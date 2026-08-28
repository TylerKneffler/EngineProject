#pragma once
#include "Core/Component.h"
#include "Core/PropertyMacros.h"
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace Engine::Components
{
struct MatrixLayerConnection
{
    bool enabled = false;
    int connectionId = 0;
    glm::vec3 boundaryPoint { 0.f };
    glm::vec3 boundaryNormal { 0.f, 0.f, 1.f };
    glm::mat4 localToRemote { 1.f };
    glm::mat4 remoteToLocal { 1.f };

    void Connect(const glm::mat4& localToRemoteTransform)
    {
        localToRemote = localToRemoteTransform;
        remoteToLocal = glm::inverse(localToRemoteTransform);
        enabled = true;
    }

    glm::vec3 TransformPoint(const glm::vec3& point) const
    {
        if (!enabled)
            return point;
        return glm::vec3(localToRemote * glm::vec4(point, 1.f));
    }

    glm::vec3 InverseTransformPoint(const glm::vec3& point) const
    {
        if (!enabled)
            return point;
        return glm::vec3(remoteToLocal * glm::vec4(point, 1.f));
    }

    bool IsPointAcrossPortal(const glm::vec3& point) const
    {
        if (!enabled)
            return false;
        return glm::dot(point - boundaryPoint, boundaryNormal) > 0.f;
    }
};

struct MatrixLayer
{
    bool enabled = false;
    glm::mat4 localToLayer { 1.f };
    glm::mat4 layerToLocal { 1.f };
    glm::vec3 portalPoint { 0.f };
    glm::vec3 portalNormal { 0.f, 0.f, 1.f };
    MatrixLayerConnection connection;

    void SetLocalToLayer(const glm::mat4& transform)
    {
        localToLayer = transform;
        layerToLocal = glm::inverse(transform);
        enabled = true;
    }

    void SetLayerToLocal(const glm::mat4& transform)
    {
        layerToLocal = transform;
        localToLayer = glm::inverse(transform);
        enabled = true;
    }

    glm::vec3 TransformPoint(const glm::vec3& point) const
    {
        if (!enabled)
            return point;
        return glm::vec3(localToLayer * glm::vec4(point, 1.f));
    }

    glm::vec3 InverseTransformPoint(const glm::vec3& point) const
    {
        if (!enabled)
            return point;
        return glm::vec3(layerToLocal * glm::vec4(point, 1.f));
    }

    bool IsPointAcrossPortal(const glm::vec3& point) const
    {
        if (!enabled)
            return false;
        return glm::dot(point - portalPoint, portalNormal) > 0.f;
    }
};

class Transform : public Engine::Core::Component
{
public:
    Transform();
    Transform(const Transform& other);
    Transform& operator=(const Transform& other);
    ~Transform() = default;

    PROPERTY(Inspector, EditAnywhere, Category = "Transform")
    glm::vec3 position { 0.f, 0.f, 0.f };
    
    PROPERTY(Inspector, EditAnywhere, Category = "Transform")
    glm::vec3 rotation { 0.f, 0.f, 0.f }; // Euler angles in radians (XYZ)
    
    PROPERTY(Inspector, EditAnywhere, Category = "Transform")
    glm::vec3 scale    { 1.f, 1.f, 1.f };

    MatrixLayer matrixLayer;
    MatrixLayerConnection portalConnection;

    // Transform fields remain public for scripts and serialization. Cache
    // accessors detect direct field edits and advance their revisions lazily.
    glm::mat4 GetLocalMatrix() const;
    glm::mat4 GetLocalMatrixWithLayer() const;

    // Returns the world matrix: parent world * T * Rz * Ry * Rx * S.
    glm::mat4 GetWorldMatrix() const;
    glm::mat4 GetWorldMatrixWithLayer() const;

    uint64_t GetLocalRevision() const;
    uint64_t GetWorldRevision() const;
    void MarkDirty();

    glm::vec3 GetWorldPosition() const;
    glm::vec3 GetLocalPosition() const;
    glm::vec3 ApplyLocalMatrixLayer(const glm::vec3& point) const;
    glm::vec3 InverseApplyLocalMatrixLayer(const glm::vec3& point) const;
    glm::vec3 ApplyWorldMatrixLayer(const glm::vec3& point) const;
    glm::vec3 InverseApplyWorldMatrixLayer(const glm::vec3& point) const;

private:
    void UpdateLocalCache() const;
    void UpdateWorldCache() const;
    static void AdvanceRevision(uint64_t& revision);

    mutable glm::vec3 m_cachedPosition { 0.f };
    mutable glm::vec3 m_cachedRotation { 0.f };
    mutable glm::vec3 m_cachedScale { 1.f };
    mutable glm::mat4 m_cachedLocalMatrix { 1.f };
    mutable glm::mat4 m_cachedWorldMatrix { 1.f };
    mutable const Engine::Core::Object* m_cachedParent = nullptr;
    mutable uint64_t m_cachedParentWorldRevision = 0;
    mutable uint64_t m_cachedLocalRevisionForWorld = 0;
    mutable uint64_t m_localRevision = 1;
    mutable uint64_t m_worldRevision = 1;
    mutable bool m_localCacheInitialized = false;
    mutable bool m_worldCacheInitialized = false;
};
}

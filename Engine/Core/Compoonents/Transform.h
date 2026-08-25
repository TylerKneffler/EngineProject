#pragma once
#include "Core/Component.h"
#include "Core/PropertyMacros.h"
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace Engine::Components
{
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

    // Transform fields remain public for scripts and serialization. Cache
    // accessors detect direct field edits and advance their revisions lazily.
    glm::mat4 GetLocalMatrix() const;

    // Returns the world matrix: parent world * T * Rz * Ry * Rx * S.
    glm::mat4 GetWorldMatrix() const;

    uint64_t GetLocalRevision() const;
    uint64_t GetWorldRevision() const;
    void MarkDirty();

    glm::vec3 GetWorldPosition() const;
    glm::vec3 GetLocalPosition() const;

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

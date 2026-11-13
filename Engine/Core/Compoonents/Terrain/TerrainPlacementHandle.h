#pragma once

#include "Core/Component.h"

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>

namespace Engine::Scene { class Scene; }

namespace Engine::Components
{
class TerrainObjectLayer;

// Editor selection proxy for one TerrainObjectLayer placement.
//
// Layer placements are plain records drawn by shared, runtime-generated
// instance batches, so they have no Object of their own to select or move.
// A runtime-only proxy Object parented to the layer carries this handle: its
// local transform mirrors the placement, so the ordinary transform gizmo,
// Properties transform fields and scene undo snapshots all work on it, and
// Synchronize() writes those edits back into the authored placement.
class TerrainPlacementHandle final : public Engine::Core::Component
{
public:
    enum class SyncResult { Unchanged, PlacementUpdated, ProxyUpdated, PlacementMissing };

    TerrainPlacementHandle();
    bool RequiresUpdate() const override { return false; }

    uint64_t placementId = 0;

    TerrainObjectLayer* GetLayer() const;
    // Proxy -> placement when the proxy transform changed since the last
    // sync; placement -> proxy when the layer changed elsewhere (brushes,
    // property edits). PlacementMissing means the record was removed.
    SyncResult Synchronize();
    // Removes the placement this handle refers to. The proxy stays alive; the
    // caller decides what to select next.
    bool RemovePlacement();

    // Finds or creates the proxy for a placement on layerObject, positioned on
    // that placement. Returns nullptr if the layer has no such placement.
    static Engine::Core::Object* Acquire(Engine::Scene::Scene& scene,
        Engine::Core::Object* layerObject, uint64_t placementId);

    // Exposed for tests: decompose a layer-local rotation into the placement's
    // surface normal and yaw. Layers with conformToGround keep their normal
    // (it drives the ground shear) and only take the yaw twist.
    static void RotationToPlacement(const glm::mat3& rotation, bool conformToGround,
        glm::vec3& normal, float& yawDegrees);

private:
    bool PullFromPlacement();
    size_t FindPlacementIndex() const;
    uint64_t m_proxyRevision = 0;
    uint64_t m_layerRevision = 0;
    mutable size_t m_indexHint = 0;
};
}

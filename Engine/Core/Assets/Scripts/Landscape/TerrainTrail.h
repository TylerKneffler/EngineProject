#pragma once
#include "Core/Components/Script.h"
#include "Core/Components/PropertyMacros.h"
#include "Core/Components/Terrain/TerrainTrails.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <string>

namespace Engine::Components { class TerrainManager; }
class SplinePath;

// A trail on a TerrainManager's ground, drawn from the SplinePath on the same
// object (its child objects are the control points). At startup, and again
// whenever the points or these settings change, the spline is sampled into a
// polyline in the manager's space and published (TerrainManager::SetTrail):
// the terrain shades the track per pixel on every level, carves it into the
// procedural cells, and ground cover and placement rules keep off it. Only
// the spline's x and z matter; the track follows the ground.
class TerrainTrail final : public Engine::Core::Script
{
public:
    TerrainTrail();

    // Object holding the TerrainManager; empty takes the scene's first.
    PROPERTY(Inspector, EditAnywhere, Category = "Trail")
    std::string terrainObjectName;
    // Half-widths from the centre line, metres: full track inside, none past
    // the edge.
    PROPERTY(Inspector, EditAnywhere, Category = "Trail", ClampMin = "0", ClampMax = "50")
    float halfWidth = 1.6f;
    PROPERTY(Inspector, EditAnywhere, Category = "Trail", ClampMin = "0", ClampMax = "60")
    float edgeHalfWidth = 4.2f;
    PROPERTY(Inspector, EditAnywhere, Category = "Trail", Range = "0, 1")
    float strength = .9f;
    // Metres the track sinks into the ground at its centre (0: drawn only).
    PROPERTY(Inspector, EditAnywhere, Category = "Trail", ClampMin = "-2", ClampMax = "2")
    float carveDepth = .12f;
    // Terrain paint layer the track shows, and the layers it may run over
    // (bit mask; it fades out over the others).
    PROPERTY(Inspector, EditAnywhere, Category = "Trail", ClampMin = "0", ClampMax = "5")
    int layer = 1;
    PROPERTY(Inspector, EditAnywhere, Category = "Trail")
    int crossableLayers = 63;
    // Colour on far tiles; negative takes the terrain's far colour of the layer.
    PROPERTY(Inspector, EditAnywhere, Category = "Trail")
    glm::vec3 farColor{-1.f};
    // Spline sampling (metres) and the largest sideways error the simplified
    // polyline may make.
    PROPERTY(Inspector, EditAnywhere, Category = "Sampling", ClampMin = "0.25", ClampMax = "16")
    float sampleSpacing = 1.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Sampling", ClampMin = "0", ClampMax = "2")
    float simplifyTolerance = .05f;

    void Start() override;
    void Update() override;
    void OnAfterDeserialize(Engine::Graphics::IGraphicsProvider* provider) override;
    void Enabled() override;
    void Disabled() override;
    void OnDestroy() override;

    // The polyline as published (manager space x, z), for tools and tests.
    const std::vector<glm::vec2>& GetPolyline() const { return m_polyline; }
    // Samples the path now and publishes it when anything changed.
    void Publish();

private:
    Engine::Components::TerrainManager* ResolveManager();
    void Withdraw();
    uint64_t Signature(const SplinePath& path, const Engine::Components::TerrainManager& manager) const;

    uint64_t m_managerObjectId = 0;
    // The manager this trail's editor watcher is registered with.
    Engine::Components::TerrainManager* m_watched = nullptr;
    uint64_t m_publishedSignature = 0;
    std::vector<glm::vec2> m_polyline;
};

#include "Scripts/Landscape/TerrainTrail.h"
#include "Core/Components/Path/SplinePath.h"
#include "Core/Components/Terrain/TerrainManager.h"
#include "Core/Scene/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <cmath>

using Engine::Components::TerrainManager;

namespace
{
struct TerrainTrailRegistration
{
    TerrainTrailRegistration()
    {
        Engine::Serialization::RegisterComponentType<TerrainTrail>("TerrainTrail");
    }
} registration;

void Mix(uint64_t& hash, const void* data, size_t size)
{
    for (size_t i = 0; i < size; ++i)
        hash = (hash ^ static_cast<const uint8_t*>(data)[i]) * 1099511628211ull;
}

// Douglas-Peucker: keeps the points that hold the line within tolerance.
void Simplify(const std::vector<glm::vec2>& points, size_t first, size_t last, float tolerance,
    std::vector<bool>& keep)
{
    if (last <= first + 1) return;
    const glm::vec2 a = points[first], b = points[last], ab = b - a;
    const float lengthSquared = glm::dot(ab, ab);
    float worst = -1.f;
    size_t worstIndex = first;
    for (size_t i = first + 1; i < last; ++i)
    {
        const float t = lengthSquared > 0.f
            ? std::clamp(glm::dot(points[i] - a, ab) / lengthSquared, 0.f, 1.f) : 0.f;
        const float distance = glm::length(points[i] - (a + ab * t));
        if (distance > worst) { worst = distance; worstIndex = i; }
    }
    if (worst <= tolerance) return;
    keep[worstIndex] = true;
    Simplify(points, first, worstIndex, tolerance, keep);
    Simplify(points, worstIndex, last, tolerance, keep);
}
}

TerrainTrail::TerrainTrail()
{
    SetTypeName(COMPONENT_TYPE_NAME(TerrainTrail));
    RegisterField("terrainObjectName", terrainObjectName, "Trail");
    RegisterField("halfWidth", halfWidth, "Trail");
    RegisterField("edgeHalfWidth", edgeHalfWidth, "Trail");
    RegisterField("strength", strength, "Trail");
    RegisterField("carveDepth", carveDepth, "Trail");
    RegisterField("layer", layer, "Trail");
    RegisterField("crossableLayers", crossableLayers, "Trail");
    RegisterField("farColor", farColor, "Trail");
    RegisterField("sampleSpacing", sampleSpacing, "Sampling");
    RegisterField("simplifyTolerance", simplifyTolerance, "Sampling");
    RegisterFieldTooltip("terrainObjectName", "Object with the Terrain Manager this trail is drawn on. Empty: the scene's first.");
    RegisterFieldTooltip("halfWidth", "Metres from the centre line where the track is full.");
    RegisterFieldTooltip("edgeHalfWidth", "Metres from the centre line where the track has faded out.");
    RegisterFieldTooltip("carveDepth", "Metres the track sinks into procedural terrain cells at its centre (0: drawn only).");
    RegisterFieldTooltip("layer", "Terrain paint layer the track shows (landscape: 0 sand, 1 gravel, 2 meadow, 3 forest soil, 4 rock, 5 snow).");
    RegisterFieldTooltip("crossableLayers", "Bit mask of the paint layers the track may run over; it fades out over the others (landscape: 14 = gravel, meadow, forest soil).");
    RegisterFieldTooltip("farColor", "Track colour on far terrain tiles. Negative: the terrain's far colour of the layer.");
    RegisterFieldTooltip("sampleSpacing", "Metres between samples of the spline before simplifying.");
    RegisterFieldTooltip("simplifyTolerance", "Largest sideways error, metres, of the published polyline against the spline.");
}

void TerrainTrail::OnAfterDeserialize(Engine::Graphics::IGraphicsProvider*)
{
    // Before the terrain's cells build when this object comes first in the
    // scene, so they carve it from the start.
    Publish();
}

void TerrainTrail::Start() { Publish(); }

void TerrainTrail::Update()
{
    // A static trail cannot move while playing: once published, skip the
    // per-frame check (60 trails rebuilt their point signatures every frame,
    // ~0.7 ms of update; todo.md Step 0). The editor moves points and changes
    // settings through the manager's trail watcher, which still republishes.
    if (m_publishedSignature && Owner && Owner->IsStaticInHierarchy()) return;
    Publish();
}

void TerrainTrail::Enabled() { m_publishedSignature = 0; Publish(); }

void TerrainTrail::Disabled() { Withdraw(); }

void TerrainTrail::OnDestroy()
{
    Withdraw();
    if (auto* scene = Owner ? Owner->GetScene() : nullptr; scene && m_managerObjectId)
        if (auto* object = scene->FindObjectById(m_managerObjectId))
            if (auto* manager = object->GetComponent<TerrainManager>())
                manager->RemoveTrailWatcher(reinterpret_cast<uintptr_t>(this));
    m_watched = nullptr;
}

TerrainManager* TerrainTrail::ResolveManager()
{
    auto* scene = Owner ? Owner->GetScene() : nullptr;
    if (!scene) return nullptr;
    if (m_managerObjectId)
        if (auto* object = scene->FindObjectById(m_managerObjectId))
            if (auto* manager = object->GetComponent<TerrainManager>()) return manager;
    TerrainManager* manager = nullptr;
    if (!terrainObjectName.empty())
    {
        if (auto* object = Owner->FindObjectInSceneByName(terrainObjectName))
            manager = object->GetComponent<TerrainManager>();
    }
    else
        for (const auto& object : scene->GetObjects())
            if (object && (manager = object->GetComponentInChildren<TerrainManager>(true))) break;
    m_managerObjectId = manager && manager->Owner ? scene->GetObjectId(manager->Owner) : 0;
    return manager;
}

uint64_t TerrainTrail::Signature(const SplinePath& path, const TerrainManager& manager) const
{
    uint64_t hash = path.GetPointSignature();
    Mix(hash, &halfWidth, sizeof(halfWidth)); Mix(hash, &edgeHalfWidth, sizeof(edgeHalfWidth));
    Mix(hash, &strength, sizeof(strength)); Mix(hash, &carveDepth, sizeof(carveDepth));
    Mix(hash, &layer, sizeof(layer)); Mix(hash, &crossableLayers, sizeof(crossableLayers));
    Mix(hash, &farColor, sizeof(farColor)); Mix(hash, &sampleSpacing, sizeof(sampleSpacing));
    Mix(hash, &simplifyTolerance, sizeof(simplifyTolerance));
    const glm::dmat4 managerWorld = manager.Owner ? manager.Owner->transform.GetWorldMatrixD() : glm::dmat4(1.0);
    Mix(hash, &managerWorld, sizeof(managerWorld));
    Mix(hash, &m_managerObjectId, sizeof(m_managerObjectId));
    return hash | 1ull;
}

void TerrainTrail::Publish()
{
    auto* path = Owner ? Owner->GetComponent<SplinePath>() : nullptr;
    auto* manager = ResolveManager();
    // The editor has no script Update: the manager calls back each frame.
    if (manager && manager != m_watched)
    {
        manager->SetTrailWatcher(reinterpret_cast<uintptr_t>(this), [this] { Publish(); });
        m_watched = manager;
    }
    if (!path || !manager || !Owner->IsEnabledInHierarchy())
    {
        Withdraw();
        return;
    }
    const uint64_t signature = Signature(*path, *manager);
    if (signature == m_publishedSignature) return;
    m_publishedSignature = signature;

    // Sample the spline evenly, take it into the manager's space and keep
    // the samples that hold the curve within the tolerance.
    m_polyline.clear();
    const float length = path->GetControlPointCount() >= 2 ? path->GetLength() : 0.f;
    if (std::isfinite(length) && length > 1.e-3f)
    {
        const size_t steps = std::clamp<size_t>(
            static_cast<size_t>(std::ceil(length / std::max(sampleSpacing, .25f))), 1, 1u << 20);
        const glm::dmat4 toManager = manager->Owner
            ? glm::inverse(manager->Owner->transform.GetWorldMatrixD()) : glm::dmat4(1.0);
        std::vector<glm::vec2> samples;
        samples.reserve(steps + 1);
        for (size_t i = 0; i <= steps; ++i)
        {
            const glm::vec3 world = path->EvaluatePosition(static_cast<float>(i) / steps);
            const glm::dvec4 local = toManager * glm::dvec4(glm::dvec3(world), 1.0);
            samples.emplace_back(static_cast<float>(local.x), static_cast<float>(local.z));
        }
        std::vector<bool> keep(samples.size(), false);
        keep.front() = keep.back() = true;
        Simplify(samples, 0, samples.size() - 1, std::max(simplifyTolerance, 0.f), keep);
        for (size_t i = 0; i < samples.size(); ++i)
            if (keep[i]) m_polyline.push_back(samples[i]);
    }
    Engine::Components::TerrainTrail trail;
    trail.points = m_polyline;
    trail.core = std::max(halfWidth, 0.f);
    trail.edge = std::max(edgeHalfWidth, trail.core + .01f);
    trail.strength = std::clamp(strength, 0.f, 1.f);
    trail.depth = carveDepth;
    trail.layer = std::clamp(layer, 0, 5);
    trail.crossableLayers = static_cast<uint32_t>(crossableLayers);
    trail.farColor = farColor;
    manager->SetTrail(reinterpret_cast<uintptr_t>(this), std::move(trail));
}

void TerrainTrail::Withdraw()
{
    if (!m_publishedSignature) return;
    m_publishedSignature = 0;
    if (auto* scene = Owner ? Owner->GetScene() : nullptr; scene && m_managerObjectId)
        if (auto* object = scene->FindObjectById(m_managerObjectId))
            if (auto* manager = object->GetComponent<TerrainManager>())
                manager->RemoveTrail(reinterpret_cast<uintptr_t>(this));
}

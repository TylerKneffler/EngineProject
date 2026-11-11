#include "Core/Compoonents/Path/SplineInstancePlacementProvider.h"

#include "Core/Compoonents/Path/SplinePath.h"
#include "Core/Compoonents/Mesh/MeshInstanceLayer.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Engine/Editor/UI/IEditorUi.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace
{
template<class T>
void HashValue(uint64_t& hash, const T& value)
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
    for (size_t index = 0; index < sizeof(T); ++index)
    {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
}

glm::vec3 Unit(glm::vec3 value, glm::vec3 fallback)
{
    const float length = glm::length(value);
    return std::isfinite(length) && length > 1.e-6f
        ? value / length : fallback;
}
}

SplineInstancePlacementProvider::SplineInstancePlacementProvider()
{
    SetTypeName(COMPONENT_TYPE_NAME(SplineInstancePlacementProvider));
    singlecomponent = true;
    RegisterField("pathObjectName", pathObjectName, "Spline Instances");
    RegisterField("spacing", spacing, "Spline Instances");
    RegisterField("uniformScale", uniformScale, "Spline Instances");
    RegisterField("offset", offset, "Spline Instances");
    RegisterField("up", up, "Spline Instances");
}

uint64_t SplineInstancePlacementProvider::BuildSignature(
    const SplinePath* path,
    const Engine::Components::MeshInstanceLayer* layer) const
{
    uint64_t hash = 1469598103934665603ull;
    const uintptr_t pathIdentity = reinterpret_cast<uintptr_t>(path);
    const uintptr_t layerIdentity = reinterpret_cast<uintptr_t>(layer);
    HashValue(hash, pathIdentity);
    HashValue(hash, layerIdentity);
    if (path) HashValue(hash, path->GetPointSignature());
    HashValue(hash, pathObjectName.size());
    for (const unsigned char character : pathObjectName)
        HashValue(hash, character);
    HashValue(hash, spacing);
    HashValue(hash, uniformScale);
    HashValue(hash, offset);
    HashValue(hash, up);
    return hash;
}

void SplineInstancePlacementProvider::PreparePlacements()
{
    if (!Owner || !Owner->GetScene()) return;
    auto* layer = Owner->GetComponent<Engine::Components::MeshInstanceLayer>();
    if (!Owner->IsEnabledInHierarchy())
    {
        if (layer) layer->SetProviderPlacements({});
        m_signature = 0;
        return;
    }
    auto* pathObject = pathObjectName.empty()
        ? Owner : Owner->FindObjectInSceneByName(pathObjectName);
    auto* path = pathObject ? pathObject->GetComponent<SplinePath>() : nullptr;
    const uint64_t signature = BuildSignature(path, layer);
    if (signature == m_signature) return;
    m_signature = signature;

    if (!layer)
    {
        m_message = "Add a MeshInstanceLayer to this object.";
        return;
    }
    if (!path || path->GetControlPointCount() < 2)
    {
        layer->SetProviderPlacements({});
        m_message = "Assign a SplinePath with at least two control points.";
        return;
    }
    if (!std::isfinite(spacing) || spacing <= 0.f ||
        !std::isfinite(uniformScale) || uniformScale <= 0.f ||
        !std::isfinite(offset.x) || !std::isfinite(offset.y) ||
        !std::isfinite(offset.z) || !std::isfinite(up.x) ||
        !std::isfinite(up.y) || !std::isfinite(up.z))
    {
        layer->SetProviderPlacements({});
        m_message = "Spacing, scale, offset and up vector must be finite; spacing and scale must be positive.";
        return;
    }

    const float length = path->GetLength(path->closed);
    if (!std::isfinite(length) || length <= 1.e-5f)
    {
        layer->SetProviderPlacements({});
        m_message = "The spline has no usable length.";
        return;
    }

    const double requestedCount = path->closed
        ? std::ceil(double(length) / spacing)
        : std::floor(double(length) / spacing) + 1.0;
    constexpr size_t maximumPlacements = 100000;
    if (!std::isfinite(requestedCount) || requestedCount < 1.0 ||
        requestedCount > double(maximumPlacements))
    {
        layer->SetProviderPlacements({});
        m_message = "Spline placement count exceeds 100,000; increase spacing.";
        return;
    }

    const glm::mat4 ownerWorld = Owner->transform.GetWorldMatrix();
    const float determinant = glm::determinant(glm::mat3(ownerWorld));
    if (!std::isfinite(determinant) || std::abs(determinant) < 1.e-8f)
    {
        layer->SetProviderPlacements({});
        m_message = "The instance layer transform must be invertible.";
        return;
    }
    const glm::mat4 worldToLocal = glm::inverse(ownerWorld);
    const glm::mat3 worldToLocalBasis(worldToLocal);
    const glm::vec3 frameUp = Unit(worldToLocalBasis * Unit(up, {0, 1, 0}),
        {0, 1, 0});
    const size_t count = static_cast<size_t>(requestedCount);
    const float step = path->closed ? length / float(count) : spacing;
    const uint64_t ownerId = Owner->GetScene()->GetObjectId(Owner);
    std::vector<Engine::Components::MeshInstanceLayer::Placement> placements;
    placements.reserve(count);
    for (size_t index = 0; index < count; ++index)
    {
        const float distance = std::min(length, float(index) * step);
        const float normalized = path->closed ? distance / length
            : (count > 1 ? distance / length : 0.f);
        const glm::vec3 worldPosition = path->EvaluatePosition(normalized,
            true, path->closed);
        const glm::vec3 worldTangent = Unit(path->EvaluateTangent(normalized,
            true, path->closed), {0, 0, 1});
        glm::vec3 forward = Unit(worldToLocalBasis * worldTangent, {0, 0, 1});
        glm::vec3 localUp = frameUp;
        if (std::abs(glm::dot(forward, localUp)) > .999f)
            localUp = std::abs(forward.y) < .9f
                ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
        glm::vec3 right = Unit(glm::cross(localUp, forward), {1, 0, 0});
        localUp = Unit(glm::cross(forward, right), localUp);
        glm::vec3 position = glm::vec3(worldToLocal * glm::vec4(worldPosition, 1.f));
        position += right * offset.x + localUp * offset.y + forward * offset.z;

        glm::mat4 transform(1.f);
        transform[0] = glm::vec4(right * uniformScale, 0.f);
        transform[1] = glm::vec4(localUp * uniformScale, 0.f);
        transform[2] = glm::vec4(forward * uniformScale, 0.f);
        transform[3] = glm::vec4(position, 1.f);
        uint64_t id = 1469598103934665603ull;
        HashValue(id, ownerId);
        HashValue(id, index);
        placements.push_back({id ? id : uint64_t(index + 1), transform});
    }
    layer->SetProviderPlacements(std::move(placements));
    m_message.clear();
}

void SplineInstancePlacementProvider::Disabled()
{
    m_signature = 0;
    if (Owner)
        if (auto* layer = Owner->GetComponent<Engine::Components::MeshInstanceLayer>())
            layer->SetProviderPlacements({});
}

void SplineInstancePlacementProvider::OnDestroy()
{
    if (Owner)
        if (auto* layer = Owner->GetComponent<Engine::Components::MeshInstanceLayer>())
            layer->SetProviderPlacements({});
}

bool SplineInstancePlacementProvider::DrawProperties(
    Engine::Editor::IEditorUi& ui)
{
    const bool changed = Component::DrawProperties(ui);
    if (changed)
    {
        m_signature = 0;
        MarkConfigurationDirty();
    }
    if (!m_message.empty()) ui.ValueLabel("Spline instances", m_message.c_str());
    return changed;
}

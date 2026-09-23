#include "Core/Compoonents/Path/SplinePath.h"

#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
float ClampPathTime(float value, bool closed)
{
    if (!closed)
        return std::clamp(value, 0.f, 1.f);
    value = std::fmod(value, 1.f);
    return value < 0.f ? value + 1.f : value;
}

void HashBytes(uint64_t& hash, const void* source, size_t byteCount)
{
    const auto* bytes = static_cast<const unsigned char*>(source);
    for (size_t index = 0; index < byteCount; ++index)
    {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
}
}

SplinePath::SplinePath()
{
    SetTypeName(COMPONENT_TYPE_NAME(SplinePath));
    RegisterField("interpolation", interpolation);
    RegisterField("closed", closed);
    RegisterField("tension", tension);
    RegisterField("arcLengthSamplesPerSegment", arcLengthSamplesPerSegment);
    RegisterField("includeDisabledPoints", includeDisabledPoints);
}

std::vector<glm::vec3> SplinePath::CollectPoints() const
{
    std::vector<glm::vec3> points;
    if (!Owner)
        return points;
    points.reserve(Owner->Children.size());
    for (const Engine::Core::Object* child : Owner->Children)
    {
        if (child && (includeDisabledPoints || child->IsEnabledInHierarchy()))
            points.push_back(child->transform.GetWorldPosition());
    }
    return points;
}

uint64_t SplinePath::BuildPointSignature(bool useClosed) const
{
    uint64_t hash = 1469598103934665603ull;
    HashBytes(hash, &interpolation, sizeof(interpolation));
    HashBytes(hash, &useClosed, sizeof(useClosed));
    HashBytes(hash, &tension, sizeof(tension));
    HashBytes(hash, &arcLengthSamplesPerSegment,
        sizeof(arcLengthSamplesPerSegment));
    HashBytes(hash, &includeDisabledPoints, sizeof(includeDisabledPoints));
    if (!Owner)
        return hash;
    for (const Engine::Core::Object* child : Owner->Children)
    {
        if (!child || (!includeDisabledPoints && !child->IsEnabledInHierarchy()))
            continue;
        const uint64_t revision = child->transform.GetWorldRevision();
        HashBytes(hash, &revision, sizeof(revision));
        const auto address = reinterpret_cast<uintptr_t>(child);
        HashBytes(hash, &address, sizeof(address));
    }
    return hash;
}

glm::vec3 SplinePath::EvaluateRaw(const std::vector<glm::vec3>& points,
    float parameter, bool useClosed) const
{
    if (points.empty())
        return Owner ? Owner->transform.GetWorldPosition() : glm::vec3(0.f);
    if (points.size() == 1u)
        return points.front();

    parameter = ClampPathTime(parameter, useClosed);
    const size_t segmentCount = useClosed ? points.size() : points.size() - 1u;
    const float scaled = parameter * static_cast<float>(segmentCount);
    const size_t segment = std::min(static_cast<size_t>(scaled),
        segmentCount - 1u);
    const float local = std::clamp(scaled - static_cast<float>(segment),
        0.f, 1.f);
    const size_t first = segment;
    const size_t second = (segment + 1u) % points.size();
    if (interpolation == static_cast<int>(Interpolation::Linear))
        return glm::mix(points[first], points[second], local);

    const glm::vec3 p1 = points[first];
    const glm::vec3 p2 = points[second];
    const glm::vec3 p0 = first > 0u ? points[first - 1u]
        : (useClosed ? points.back() : p1 - (p2 - p1));
    const size_t afterSecond = second + 1u;
    const glm::vec3 p3 = afterSecond < points.size() ? points[afterSecond]
        : (useClosed ? points[afterSecond % points.size()] : p2 + (p2 - p1));
    const float tangentScale = 0.5f * (1.f - std::clamp(tension, 0.f, 1.f));
    const glm::vec3 m1 = tangentScale * (p2 - p0);
    const glm::vec3 m2 = tangentScale * (p3 - p1);
    const float t2 = local * local;
    const float t3 = t2 * local;
    return (2.f * t3 - 3.f * t2 + 1.f) * p1 +
        (t3 - 2.f * t2 + local) * m1 +
        (-2.f * t3 + 3.f * t2) * p2 + (t3 - t2) * m2;
}

glm::vec3 SplinePath::EvaluateRawTangent(
    const std::vector<glm::vec3>& points, float parameter,
    bool useClosed) const
{
    if (points.size() < 2u)
        return { 0.f, 0.f, 1.f };
    const float epsilon = 1.f / static_cast<float>(
        std::max<size_t>(256u, points.size() * 64u));
    const float before = useClosed ? parameter - epsilon
        : std::max(0.f, parameter - epsilon);
    const float after = useClosed ? parameter + epsilon
        : std::min(1.f, parameter + epsilon);
    const glm::vec3 tangent = EvaluateRaw(points, after, useClosed) -
        EvaluateRaw(points, before, useClosed);
    const float length = glm::length(tangent);
    return length > 0.000001f ? tangent / length : glm::vec3(0.f, 0.f, 1.f);
}

void SplinePath::EnsureArcLengthCache(
    const std::vector<glm::vec3>& points, bool useClosed) const
{
    const uint64_t signature = BuildPointSignature(useClosed);
    if (signature == m_cachedSignature && !m_arcSamples.empty())
        return;
    m_cachedSignature = signature;
    m_arcSamples.clear();
    m_cachedLength = 0.f;
    if (points.size() < 2u)
    {
        m_arcSamples.push_back({ 0.f, 0.f });
        return;
    }
    const size_t segmentCount = useClosed ? points.size() : points.size() - 1u;
    const size_t sampleCount = segmentCount * static_cast<size_t>(
        std::clamp(arcLengthSamplesPerSegment, 4, 128));
    m_arcSamples.reserve(sampleCount + 1u);
    glm::vec3 previous = EvaluateRaw(points, 0.f, useClosed);
    m_arcSamples.push_back({ 0.f, 0.f });
    for (size_t index = 1; index <= sampleCount; ++index)
    {
        const float parameter = static_cast<float>(index) /
            static_cast<float>(sampleCount);
        // A closed curve evaluates 1 as 0, which is exactly the final closing
        // sample needed for complete length and seam continuity.
        const glm::vec3 position = EvaluateRaw(points, parameter, useClosed);
        m_cachedLength += glm::length(position - previous);
        m_arcSamples.push_back({ parameter, m_cachedLength });
        previous = position;
    }
}

float SplinePath::DistanceToParameter(float normalizedDistance) const
{
    if (m_arcSamples.size() < 2u || m_cachedLength <= 0.000001f)
        return std::clamp(normalizedDistance, 0.f, 1.f);
    const float target = std::clamp(normalizedDistance, 0.f, 1.f) *
        m_cachedLength;
    const auto found = std::lower_bound(m_arcSamples.begin(),
        m_arcSamples.end(), target,
        [](const ArcSample& sample, float distance)
        {
            return sample.distance < distance;
        });
    if (found == m_arcSamples.begin())
        return found->parameter;
    if (found == m_arcSamples.end())
        return 1.f;
    const ArcSample& next = *found;
    const ArcSample& previous = *(found - 1);
    const float span = next.distance - previous.distance;
    const float amount = span > 0.000001f
        ? (target - previous.distance) / span : 0.f;
    return glm::mix(previous.parameter, next.parameter, amount);
}

glm::vec3 SplinePath::EvaluatePosition(float normalizedTime,
    bool constantSpeed, bool forceClosed) const
{
    const std::vector<glm::vec3> points = CollectPoints();
    const bool useClosed = closed || forceClosed;
    EnsureArcLengthCache(points, useClosed);
    float parameter = ClampPathTime(normalizedTime, useClosed);
    if (constantSpeed)
        parameter = DistanceToParameter(parameter);
    return EvaluateRaw(points, parameter, useClosed);
}

glm::vec3 SplinePath::EvaluateTangent(float normalizedTime,
    bool constantSpeed, bool forceClosed) const
{
    const std::vector<glm::vec3> points = CollectPoints();
    const bool useClosed = closed || forceClosed;
    EnsureArcLengthCache(points, useClosed);
    float parameter = ClampPathTime(normalizedTime, useClosed);
    if (constantSpeed)
        parameter = DistanceToParameter(parameter);
    return EvaluateRawTangent(points, parameter, useClosed);
}

float SplinePath::GetLength(bool forceClosed) const
{
    const std::vector<glm::vec3> points = CollectPoints();
    EnsureArcLengthCache(points, closed || forceClosed);
    return m_cachedLength;
}

size_t SplinePath::GetControlPointCount() const
{
    return CollectPoints().size();
}

bool SplinePath::DrawProperties(Engine::Editor::IEditorUi& ui)
{
    bool changed = false;
    static const char* interpolationNames[] = { "Linear", "Catmull-Rom" };
    changed = ui.Combo("Interpolation", &interpolation,
        interpolationNames, 2) || changed;
    changed = ui.Checkbox("Closed Loop", &closed) || changed;
    if (interpolation == static_cast<int>(Interpolation::CatmullRom))
        changed = ui.SliderFloat("Tension", &tension, 0.f, 1.f) || changed;
    changed = ui.SliderInt("Arc Samples / Segment",
        &arcLengthSamplesPerSegment, 4, 128) || changed;
    changed = ui.Checkbox("Include Disabled Points",
        &includeDisabledPoints) || changed;
    if (Owner && Owner->GetScene() && ui.Button("Add Control Point"))
    {
        Engine::Core::Object* point = Owner->GetScene()->AddObject(
            "Point " + std::to_string(Owner->Children.size() + 1u));
        point->Parent = Owner;
        Owner->Children.push_back(point);
        point->transform.position = Owner->Children.size() > 1u
            ? Owner->Children[Owner->Children.size() - 2u]->transform.position +
                glm::vec3(0.f, 0.f, 2.f)
            : glm::vec3(0.f);
        changed = true;
    }
    char summary[96]{};
    std::snprintf(summary, sizeof(summary), "%zu points, %.2f units",
        GetControlPointCount(), GetLength());
    ui.ValueLabel("Path", summary);
    ui.DisabledLabel("Direct children define control-point order.");
    return changed;
}

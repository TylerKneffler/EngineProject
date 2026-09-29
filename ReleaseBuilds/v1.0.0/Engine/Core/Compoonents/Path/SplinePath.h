#pragma once

#include "Core/PropertyMacros.h"
#include "Core/component.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

// General-purpose scene-editable world-space path. Direct child objects are
// its ordered control points, usable by cameras or any ordinary scene object.
class SplinePath final : public Engine::Core::Component
{
public:
    enum class Interpolation : int { Linear = 0, CatmullRom = 1 };
    SplinePath();

    PROPERTY(Inspector, EditAnywhere, Category = "Spline")
    int interpolation = static_cast<int>(Interpolation::CatmullRom);
    PROPERTY(Inspector, EditAnywhere, Category = "Spline")
    bool closed = false;
    PROPERTY(Inspector, EditAnywhere, Category = "Spline", Range = "0, 1")
    float tension = 0.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Spline", ClampMin = "4", ClampMax = "128")
    int arcLengthSamplesPerSegment = 24;
    PROPERTY(Inspector, EditAnywhere, Category = "Spline")
    bool includeDisabledPoints = false;

    // forceClosed is used by continuous-loop followers. It closes even an
    // authored open path without modifying the reusable path component.
    glm::vec3 EvaluatePosition(float normalizedTime,
        bool constantSpeed = true, bool forceClosed = false) const;
    glm::vec3 EvaluateTangent(float normalizedTime,
        bool constantSpeed = true, bool forceClosed = false) const;
    float GetLength(bool forceClosed = false) const;
    size_t GetControlPointCount() const;
    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;

private:
    struct ArcSample { float parameter = 0.f; float distance = 0.f; };
    std::vector<glm::vec3> CollectPoints() const;
    void EnsureArcLengthCache(const std::vector<glm::vec3>& points,
        bool useClosed) const;
    glm::vec3 EvaluateRaw(const std::vector<glm::vec3>& points,
        float parameter, bool useClosed) const;
    glm::vec3 EvaluateRawTangent(const std::vector<glm::vec3>& points,
        float parameter, bool useClosed) const;
    float DistanceToParameter(float normalizedDistance) const;
    uint64_t BuildPointSignature(bool useClosed) const;

    mutable uint64_t m_cachedSignature = 0;
    mutable std::vector<ArcSample> m_arcSamples;
    mutable float m_cachedLength = 0.f;
};

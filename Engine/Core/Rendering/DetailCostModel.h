#pragma once

#include <array>
#include <memory>
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

namespace Engine::Rendering
{
class OcclusionBuffer;

// Predicts what the visible LOD'd instances of one view would cost at every
// detail scale, so the detail budget can choose the scale directly instead
// of nudging it by measured time alone.
//
// Each placement is weighted by how visible it is. Placements are processed
// front to back over a coarse screen coverage grid: a tree behind other trees
// counts only for the part of it still uncovered. Detail therefore goes where
// it shows. One near tree and a hundred open-field trees far away is cheap,
// so the far trees get full detail; a deep forest, where most trunks hide
// behind the first rows, is not charged for the hidden ones.
//
// Cost is in visibility-weighted triangles. The budget converts it to GPU
// time through its measured feedback (DetailBudget::ReportGeometryCost).
struct DetailCostInput
{
    struct Band
    {
        // Unscaled LOD distance range of this band; end may be huge (last band).
        float start = 0.f, end = 1.e20f;
        double triangles = 0.0;
    };
    struct Rows { glm::vec4 rows[3]; };
    // One instanced tile: its placements and its LOD bands.
    struct Group
    {
        glm::mat4 world{ 1.f };
        // Bounding sphere of the source mesh, in placement space.
        glm::vec3 localCenter{ 0.f };
        float localRadius = 1.f;
        // Layer budget weight: layer LOD scale = global ^ weight.
        float weight = 1.f;
        // How much a placement hides what is behind it (foliage lets light through).
        float opacity = 1.f;
        std::vector<Band> bands;
        std::vector<Rows> transforms;
    };
    std::vector<Group> groups;
    glm::mat4 viewProjection{ 1.f };
    glm::vec3 cameraPosition{ 0.f };
    // Optional: placements hidden behind terrain cost nothing.
    std::shared_ptr<const OcclusionBuffer> occlusion;
    // Screen coverage (ScreenCoverage.h): a placement scaled by s keeps each
    // band out to s times the distance, as the GPU cull and Object.hlsl do.
    // Band distances arrive already scaled by the view's coverage scale.
    bool placementScaleLod = false;
    float viewportWidth = 1920.f, viewportHeight = 1080.f;
};

struct DetailCostCurve
{
    // cost[i] is the predicted cost at global scale exp(LogScale(i)).
    static constexpr int Buckets = 160;
    static constexpr float MinimumLog = -2.3f;  // scale 0.1
    static constexpr float MaximumLog = 3.0f;   // scale 20
    static constexpr float Step = (MaximumLog - MinimumLog) / Buckets;
    static float LogScale(int bucket) { return MinimumLog + (bucket + .5f) * Step; }

    std::array<double, Buckets> cost{};
    uint32_t placements = 0;       // in the view frustum
    double occludedFraction = 0.0; // share of placement visibility lost to cover
    bool valid = false;

    // Predicted cost at a global scale (linear between buckets).
    double CostAt(float scale) const;
    // The largest scale within [minimum, maximum] whose cost fits the budget.
    float LargestScaleWithin(double budget, float minimum, float maximum) const;
};

DetailCostCurve BuildDetailCostCurve(const DetailCostInput& input);
}

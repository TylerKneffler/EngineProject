#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

// Region editing for terrain: a selection mask (brush circle, rectangle,
// ellipse or free polygon, with a feathered edge), a height window that spans
// any number of cells, and whole-region operations such as hydraulic and
// thermal erosion that need neighbours beyond one cell.
//
// Terrain::EditRegion and TerrainManager::EditRegion gather the heights under
// the mask into one TerrainHeightGrid, run an operation, and write every
// sample back to all cells that share it, so no operation can open a seam.
// Positions are in the root's local space (the Terrain's object, or the
// manager's), on the X/Z plane.
namespace Engine::Components
{
struct TerrainRegionMask
{
    enum class Shape { Circle, Rectangle, Ellipse, Polygon };
    Shape shape = Shape::Circle;
    // Circle: center and halfSize.x radius. Rectangle / ellipse: center and
    // half extents. Polygon: points (closed implicitly; lasso or clicked).
    glm::dvec2 center{0.0};
    glm::dvec2 halfSize{0.0};   // zero: an empty mask
    std::vector<glm::dvec2> points;
    // Soft edge, in metres inside the shape, over which the weight rises from
    // 0 to 1 (smoothstep).
    float feather = 0.f;

    // A brush circle whose hardness is the share of the radius at full weight.
    static TerrainRegionMask Circle(glm::dvec2 center, float radius, float hardness);
    static TerrainRegionMask Box(glm::dvec2 a, glm::dvec2 b, bool ellipse, float feather);
    static TerrainRegionMask Polygon(std::vector<glm::dvec2> points, float feather);

    bool Empty() const;
    // 0 outside, 1 deeper inside than the feather.
    float Weight(double x, double z) const;
    // Distance inside the shape (negative outside).
    double Inside(double x, double z) const;
    bool Bounds(glm::dvec2& minimum, glm::dvec2& maximum) const;
    // Closed outline, about segments points long (exact corners for polygons).
    std::vector<glm::dvec2> Outline(int segments = 96) const;
};

struct TerrainHeightGrid
{
    double originX = 0.0, originZ = 0.0; // local position of sample (0, 0)
    float spacingX = 1.f, spacingZ = 1.f;
    int width = 0, height = 0;
    std::vector<float> heights;
    // 1 where a loaded cell supplied the sample. Missing samples are filled
    // from their neighbours so operations can read them, and never written.
    std::vector<uint8_t> valid;

    float& At(int col, int row) { return heights[static_cast<size_t>(row) * width + col]; }
    float At(int col, int row) const { return heights[static_cast<size_t>(row) * width + col]; }
    double X(int col) const { return originX + double(col) * spacingX; }
    double Z(int row) const { return originZ + double(row) * spacingZ; }
};

struct TerrainRegionOperation
{
    enum class Kind
    {
        Raise,      // + amount metres (Lower: negative amount)
        Flatten,    // toward height
        Smooth,     // iterations of a 3x3 blur
        Noise,      // fractal noise, amplitude metres at noiseScale metres
        Terrace,    // steps of terraceStep metres, terraceSharpness 0..1
        Hydraulic,  // particle erosion: droplets carve channels, settle sediment
        Thermal,    // slopes steeper than talusDegrees slump into scree
    };
    Kind kind = Kind::Hydraulic;
    // Blend of the result over the mask, 0..1.
    float strength = 1.f;
    float amount = 1.f;
    float height = 0.f;
    int iterations = 4;
    float noiseScale = 40.f;
    float noiseAmplitude = 2.f;
    float terraceStep = 4.f;
    float terraceSharpness = .7f;
    // Hydraulic: droplets per square metre of mask weight (the count is
    // capped at maxDroplets), and the usual droplet rules.
    float dropletDensity = .5f;
    int maxDroplets = 400000;
    int maxSteps = 64;
    float inertia = .1f;
    float capacity = 4.f;
    float erodeSpeed = .3f;
    float depositSpeed = .3f;
    float evaporation = .02f;
    int erosionRadius = 2;      // samples
    // Height unit for the droplet rules, in multiples of the grid spacing.
    // Larger is gentler (see Tools/build_landscape_demo.py, erode()).
    float erosionUnit = 60.f;
    float talusDegrees = 35.f;
    uint32_t seed = 1;

    // Samples of context the operation reads beyond the mask, in metres for
    // a grid of the given spacing.
    float Margin(float spacing) const;
};

// Runs the operation on the grid and blends it in by mask weight * strength.
// Returns whether any sample changed.
bool ApplyRegionOperation(TerrainHeightGrid& grid, const TerrainRegionMask& mask,
    const TerrainRegionOperation& operation);

// Fills invalid samples from their nearest valid neighbours along rows and
// columns (called by EditRegion before the operation).
void FillMissingSamples(TerrainHeightGrid& grid);

}

#include "TerrainRegionEdit.h"

#include "Core/Compoonents/Terrain/Terrain.h"
#include "Core/Compoonents/Terrain/TerrainManager.h"
#include "Core/Scene/Object.h"
#include "Core/Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Engine::Components
{
namespace
{
constexpr double Pi = 3.14159265358979323846;

float SmoothStep(float a, float b, float x)
{
    if (b <= a) return x >= b ? 1.f : 0.f;
    const float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

double SegmentDistance(glm::dvec2 p, glm::dvec2 a, glm::dvec2 b)
{
    const glm::dvec2 ab = b - a;
    const double length = glm::dot(ab, ab);
    const double t = length > 0.0 ? std::clamp(glm::dot(p - a, ab) / length, 0.0, 1.0) : 0.0;
    return glm::length(p - (a + ab * t));
}

uint32_t Hash(int32_t x, int32_t z, uint32_t seed)
{
    uint32_t h = static_cast<uint32_t>(x) * 0x9e3779b1u ^
        static_cast<uint32_t>(z) * 0x85ebca77u ^ seed * 0xc2b2ae3du;
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
    return h;
}

// Gradient noise in about -0.7..0.7 over local metres, so the same position
// gets the same value in every cell and every stroke.
float GradientNoise(double x, double z, uint32_t seed)
{
    const double fx = std::floor(x), fz = std::floor(z);
    const int32_t ix = static_cast<int32_t>(fx), iz = static_cast<int32_t>(fz);
    const double tx = x - fx, tz = z - fz;
    const auto corner = [&](int dx, int dz) {
        const double angle = (Hash(ix + dx, iz + dz, seed) & 0xffffffu) / double(0x1000000) * 2.0 * Pi;
        return std::cos(angle) * (tx - dx) + std::sin(angle) * (tz - dz);
    };
    const auto fade = [](double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); };
    const double ux = fade(tx), uz = fade(tz);
    const double bottom = corner(0, 0) * (1 - ux) + corner(1, 0) * ux;
    const double top = corner(0, 1) * (1 - ux) + corner(1, 1) * ux;
    return float(bottom * (1 - uz) + top * uz);
}

float FractalNoise(double x, double z, double period, uint32_t seed)
{
    float total = 0.f, amplitude = 1.f, norm = 0.f;
    for (int octave = 0; octave < 5; ++octave)
    {
        // Rotate each octave so the lattice directions do not line up.
        const double angle = .61 + octave * 1.13;
        const double c = std::cos(angle), s = std::sin(angle);
        const double scale = period / double(1 << octave);
        total += amplitude * GradientNoise((c * x - s * z) / scale, (s * x + c * z) / scale,
            seed + uint32_t(octave) * 17u);
        norm += amplitude;
        amplitude *= .5f;
    }
    return total / norm;
}

struct Random
{
    uint32_t state;
    float Next()
    {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        return (state & 0xffffffu) / float(0x1000000);
    }
};

// Particle hydraulic erosion on a copy of the grid (in place). Droplets start
// where the mask allows, in proportion to its weight. See
// Tools/build_landscape_demo.py, erode(), for the same rules in numpy.
void Hydraulic(TerrainHeightGrid& grid, const std::vector<float>& weight,
    const TerrainRegionOperation& op, double maskArea)
{
    const int width = grid.width, height = grid.height;
    const int radius = std::clamp(op.erosionRadius, 1, 8);
    const int margin = radius + 1;
    if (width <= 2 * margin + 2 || height <= 2 * margin + 2) return;
    const double spacing = std::min(grid.spacingX, grid.spacingZ);
    const double unit = std::max(1.f, op.erosionUnit) * spacing;
    std::vector<double> h(grid.heights.size());
    for (size_t i = 0; i < h.size(); ++i) h[i] = grid.heights[i] / unit;
    struct Tap { int offset; double weight; };
    std::vector<Tap> brush;
    double brushSum = 0.0;
    for (int oy = -radius; oy <= radius; ++oy)
        for (int ox = -radius; ox <= radius; ++ox)
        {
            const double distance = std::sqrt(double(ox * ox + oy * oy));
            if (distance >= radius) continue;
            brush.push_back({oy * width + ox, radius - distance});
            brushSum += radius - distance;
        }
    for (auto& tap : brush) tap.weight /= brushSum;
    const auto sample = [&](double px, double py, double& gx, double& gy) {
        const int ix = int(px), iy = int(py);
        const double fx = px - ix, fy = py - iy;
        const size_t i = size_t(iy) * width + ix;
        const double h00 = h[i], h10 = h[i + 1], h01 = h[i + width], h11 = h[i + width + 1];
        gx = (h10 - h00) * (1 - fy) + (h11 - h01) * fy;
        gy = (h01 - h00) * (1 - fx) + (h11 - h10) * fx;
        return h00 * (1 - fx) * (1 - fy) + h10 * fx * (1 - fy) + h01 * (1 - fx) * fy + h11 * fx * fy;
    };
    const int droplets = int(std::min<double>(op.maxDroplets,
        std::max(0.f, op.dropletDensity) * maskArea));
    Random random{op.seed * 2654435761u | 1u};
    int started = 0;
    for (int attempt = 0; started < droplets && attempt < droplets * 20; ++attempt)
    {
        double px = margin + random.Next() * (width - 1 - 2 * margin);
        double py = margin + random.Next() * (height - 1 - 2 * margin);
        const size_t start = size_t(std::lround(py)) * width + size_t(std::lround(px));
        if (random.Next() >= weight[start]) continue;
        ++started;
        double dx = 0.0, dy = 0.0, speed = 1.0, water = 1.0, sediment = 0.0;
        for (int step = 0; step < op.maxSteps; ++step)
        {
            const int ix = int(px), iy = int(py);
            const double fx = px - ix, fy = py - iy;
            const size_t i = size_t(iy) * width + ix;
            double gx = 0.0, gy = 0.0;
            const double value = sample(px, py, gx, gy);
            dx = dx * op.inertia - gx * (1 - op.inertia);
            dy = dy * op.inertia - gy * (1 - op.inertia);
            const double length = std::sqrt(dx * dx + dy * dy);
            const double nx = px + (length > 1e-12 ? dx / length : 0.0);
            const double ny = py + (length > 1e-12 ? dy / length : 0.0);
            const bool inside = length > 1e-12 && nx >= margin && ny >= margin &&
                nx < width - 1 - margin && ny < height - 1 - margin;
            const auto deposit = [&](double amount) {
                h[i] += amount * (1 - fx) * (1 - fy);
                h[i + 1] += amount * fx * (1 - fy);
                h[i + width] += amount * (1 - fx) * fy;
                h[i + width + 1] += amount * fx * fy;
            };
            if (!inside) { deposit(sediment); break; }
            dx /= length; dy /= length;
            double ignored = 0.0;
            const double deltaH = sample(nx, ny, ignored, ignored) - value;
            const double cap = std::max(-deltaH * speed * water * op.capacity, .01);
            if (sediment > cap || deltaH > 0.0)
            {
                const double amount = deltaH > 0.0 ? std::min(deltaH, sediment)
                    : (sediment - cap) * op.depositSpeed;
                sediment -= amount;
                deposit(amount);
            }
            else
            {
                const double amount = std::min((cap - sediment) * op.erodeSpeed, -deltaH);
                for (const auto& tap : brush) h[i + tap.offset] -= amount * tap.weight;
                sediment += amount;
            }
            speed = std::sqrt(std::max(0.0, speed * speed - deltaH * 4.0));
            water *= 1.0 - op.evaporation;
            px = nx; py = ny;
        }
    }
    for (size_t i = 0; i < h.size(); ++i) grid.heights[i] = float(h[i] * unit);
}

// Slopes steeper than the talus angle shed material to lower neighbours.
void Thermal(TerrainHeightGrid& grid, const std::vector<float>& weight,
    const TerrainRegionOperation& op)
{
    const int width = grid.width, height = grid.height;
    const double talus = std::tan(std::clamp(op.talusDegrees, 1.f, 89.f) * Pi / 180.0);
    static constexpr int offsets[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1},
        {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    double limit[8];
    for (int n = 0; n < 8; ++n)
        limit[n] = talus * std::hypot(offsets[n][0] * grid.spacingX, offsets[n][1] * grid.spacingZ);
    std::vector<float> change(grid.heights.size());
    for (int iteration = 0; iteration < std::clamp(op.iterations, 1, 200); ++iteration)
    {
        std::fill(change.begin(), change.end(), 0.f);
        for (int row = 1; row < height - 1; ++row)
            for (int col = 1; col < width - 1; ++col)
            {
                const size_t i = size_t(row) * width + col;
                if (weight[i] <= 0.f) continue;
                const float here = grid.heights[i];
                double excess[8], total = 0.0, largest = 0.0;
                for (int n = 0; n < 8; ++n)
                {
                    const size_t j = i + offsets[n][1] * width + offsets[n][0];
                    excess[n] = std::max(0.0, double(here - grid.heights[j]) - limit[n]);
                    total += excess[n];
                    largest = std::max(largest, excess[n]);
                }
                if (total <= 0.0) continue;
                // Move half the largest excess, split by each neighbour's share.
                const double moved = .5 * largest;
                change[i] -= float(moved);
                for (int n = 0; n < 8; ++n)
                    if (excess[n] > 0.0)
                        change[i + offsets[n][1] * width + offsets[n][0]] +=
                            float(moved * excess[n] / total);
            }
        for (size_t i = 0; i < change.size(); ++i) grid.heights[i] += change[i];
    }
}

void Smooth(TerrainHeightGrid& grid, int iterations)
{
    std::vector<float> next(grid.heights.size());
    for (int iteration = 0; iteration < std::clamp(iterations, 1, 200); ++iteration)
    {
        for (int row = 0; row < grid.height; ++row)
            for (int col = 0; col < grid.width; ++col)
            {
                float sum = 0.f; int count = 0;
                for (int oy = -1; oy <= 1; ++oy)
                    for (int ox = -1; ox <= 1; ++ox)
                    {
                        const int x = col + ox, y = row + oy;
                        if (x < 0 || y < 0 || x >= grid.width || y >= grid.height) continue;
                        sum += grid.At(x, y); ++count;
                    }
                next[size_t(row) * grid.width + col] = sum / count;
            }
        grid.heights.swap(next);
    }
}
}

TerrainRegionMask TerrainRegionMask::Circle(glm::dvec2 center, float radius, float hardness)
{
    TerrainRegionMask mask;
    mask.shape = Shape::Circle;
    mask.center = center;
    mask.halfSize = glm::dvec2(radius);
    mask.feather = radius * (1.f - std::clamp(hardness, 0.f, 1.f));
    return mask;
}

TerrainRegionMask TerrainRegionMask::Box(glm::dvec2 a, glm::dvec2 b, bool ellipse, float feather)
{
    TerrainRegionMask mask;
    mask.shape = ellipse ? Shape::Ellipse : Shape::Rectangle;
    mask.center = (a + b) * .5;
    mask.halfSize = glm::abs(b - a) * .5;
    mask.feather = std::max(0.f, feather);
    return mask;
}

TerrainRegionMask TerrainRegionMask::Polygon(std::vector<glm::dvec2> points, float feather)
{
    TerrainRegionMask mask;
    mask.shape = Shape::Polygon;
    mask.points = std::move(points);
    mask.feather = std::max(0.f, feather);
    return mask;
}

bool TerrainRegionMask::Empty() const
{
    if (shape == Shape::Polygon) return points.size() < 3;
    return !(halfSize.x > 1e-6 && (shape == Shape::Circle || halfSize.y > 1e-6));
}

double TerrainRegionMask::Inside(double x, double z) const
{
    const glm::dvec2 p(x, z);
    switch (shape)
    {
    case Shape::Circle:
        return halfSize.x - glm::length(p - center);
    case Shape::Rectangle:
    {
        const glm::dvec2 d = halfSize - glm::abs(p - center);
        return std::min(d.x, d.y);
    }
    case Shape::Ellipse:
    {
        // Scaled radial distance: exact on the axes, close enough between.
        const glm::dvec2 q = (p - center) / glm::max(halfSize, glm::dvec2(1e-9));
        return (1.0 - glm::length(q)) * std::min(halfSize.x, halfSize.y);
    }
    case Shape::Polygon:
    {
        if (points.size() < 3) return -std::numeric_limits<double>::infinity();
        bool inside = false;
        double distance = std::numeric_limits<double>::infinity();
        for (size_t i = 0, j = points.size() - 1; i < points.size(); j = i++)
        {
            const glm::dvec2 a = points[i], b = points[j];
            if ((a.y > z) != (b.y > z) && x < (b.x - a.x) * (z - a.y) / (b.y - a.y) + a.x)
                inside = !inside;
            distance = std::min(distance, SegmentDistance(p, a, b));
        }
        return inside ? distance : -distance;
    }
    }
    return -1.0;
}

float TerrainRegionMask::Weight(double x, double z) const
{
    if (Empty()) return 0.f;
    const double inside = Inside(x, z);
    if (inside < 0.0) return 0.f;
    if (feather <= 0.f) return 1.f;
    return SmoothStep(0.f, feather, float(inside));
}

bool TerrainRegionMask::Bounds(glm::dvec2& minimum, glm::dvec2& maximum) const
{
    if (Empty()) return false;
    if (shape == Shape::Polygon)
    {
        minimum = maximum = points.front();
        for (const auto& point : points)
        {
            minimum = glm::min(minimum, point);
            maximum = glm::max(maximum, point);
        }
        return true;
    }
    const glm::dvec2 extent = shape == Shape::Circle ? glm::dvec2(halfSize.x) : halfSize;
    minimum = center - extent;
    maximum = center + extent;
    return true;
}

std::vector<glm::dvec2> TerrainRegionMask::Outline(int segments) const
{
    std::vector<glm::dvec2> outline;
    if (Empty()) return outline;
    segments = std::max(8, segments);
    if (shape == Shape::Polygon) return points;
    if (shape == Shape::Rectangle)
    {
        // Subdivided edges so the outline drapes over the surface.
        const glm::dvec2 corners[4] = {center + glm::dvec2(-halfSize.x, -halfSize.y),
            center + glm::dvec2(halfSize.x, -halfSize.y), center + halfSize,
            center + glm::dvec2(-halfSize.x, halfSize.y)};
        const int perEdge = std::max(2, segments / 4);
        for (int edge = 0; edge < 4; ++edge)
            for (int i = 0; i < perEdge; ++i)
                outline.push_back(glm::mix(corners[edge], corners[(edge + 1) % 4],
                    double(i) / perEdge));
        return outline;
    }
    const glm::dvec2 radii = shape == Shape::Circle ? glm::dvec2(halfSize.x) : halfSize;
    for (int i = 0; i < segments; ++i)
    {
        const double angle = 2.0 * Pi * i / segments;
        outline.push_back(center + glm::dvec2(std::cos(angle), std::sin(angle)) * radii);
    }
    return outline;
}

float TerrainRegionOperation::Margin(float spacing) const
{
    switch (kind)
    {
    case Kind::Smooth:
    case Kind::Thermal: return spacing * float(std::clamp(iterations, 1, 200) + 1);
    // Droplets need room to run past the mask edge and settle.
    case Kind::Hydraulic: return spacing * float(std::clamp(maxSteps / 2, 4, 48) + erosionRadius + 2);
    default: return spacing;
    }
}

void FillMissingSamples(TerrainHeightGrid& grid)
{
    if (grid.valid.empty()) return;
    const size_t count = grid.heights.size();
    if (std::all_of(grid.valid.begin(), grid.valid.end(), [](uint8_t v) { return v != 0; })) return;
    std::vector<uint8_t> known = grid.valid;
    // Sweep rows then columns both ways, copying the last known value; a few
    // passes reach every sample connected to loaded terrain.
    for (int pass = 0; pass < 4; ++pass)
    {
        for (int row = 0; row < grid.height; ++row)
            for (int direction = 0; direction < 2; ++direction)
            {
                bool have = false; float last = 0.f;
                for (int k = 0; k < grid.width; ++k)
                {
                    const int col = direction ? grid.width - 1 - k : k;
                    const size_t i = size_t(row) * grid.width + col;
                    if (known[i]) { have = true; last = grid.heights[i]; }
                    else if (have) { grid.heights[i] = last; known[i] = 1; }
                }
            }
        for (int col = 0; col < grid.width; ++col)
            for (int direction = 0; direction < 2; ++direction)
            {
                bool have = false; float last = 0.f;
                for (int k = 0; k < grid.height; ++k)
                {
                    const int row = direction ? grid.height - 1 - k : k;
                    const size_t i = size_t(row) * grid.width + col;
                    if (known[i]) { have = true; last = grid.heights[i]; }
                    else if (have) { grid.heights[i] = last; known[i] = 1; }
                }
            }
        if (std::all_of(known.begin(), known.end(), [](uint8_t v) { return v != 0; })) break;
    }
    (void)count;
}

bool ApplyRegionOperation(TerrainHeightGrid& grid, const TerrainRegionMask& mask,
    const TerrainRegionOperation& op)
{
    if (grid.width <= 0 || grid.height <= 0 ||
        grid.heights.size() != size_t(grid.width) * grid.height || mask.Empty()) return false;
    std::vector<float> weight(grid.heights.size());
    double maskArea = 0.0;
    const double sampleArea = double(grid.spacingX) * grid.spacingZ;
    bool any = false;
    for (int row = 0; row < grid.height; ++row)
        for (int col = 0; col < grid.width; ++col)
        {
            const size_t i = size_t(row) * grid.width + col;
            float w = mask.Weight(grid.X(col), grid.Z(row));
            if (!grid.valid.empty() && !grid.valid[i]) w = 0.f;
            weight[i] = w;
            maskArea += w * sampleArea;
            any |= w > 0.f;
        }
    if (!any) return false;
    const std::vector<float> before = grid.heights;
    switch (op.kind)
    {
    case TerrainRegionOperation::Kind::Raise:
        for (float& h : grid.heights) h += op.amount;
        break;
    case TerrainRegionOperation::Kind::Flatten:
        for (float& h : grid.heights) h = op.height;
        break;
    case TerrainRegionOperation::Kind::Smooth:
        Smooth(grid, op.iterations);
        break;
    case TerrainRegionOperation::Kind::Noise:
        for (int row = 0; row < grid.height; ++row)
            for (int col = 0; col < grid.width; ++col)
                grid.At(col, row) += op.noiseAmplitude * 1.4f *
                    FractalNoise(grid.X(col), grid.Z(row), std::max(.5f, op.noiseScale), op.seed);
        break;
    case TerrainRegionOperation::Kind::Terrace:
    {
        const float step = std::max(.05f, op.terraceStep);
        const float flat = std::clamp(op.terraceSharpness, 0.f, .98f);
        for (float& h : grid.heights)
        {
            const float level = std::floor(h / step);
            const float f = h / step - level;
            // Flat treads for the first share of each step, then a smooth riser.
            const float riser = SmoothStep(0.f, 1.f, std::max(0.f, (f - flat) / (1.f - flat)));
            h = (level + riser) * step;
        }
        break;
    }
    case TerrainRegionOperation::Kind::Hydraulic:
        Hydraulic(grid, weight, op, maskArea);
        break;
    case TerrainRegionOperation::Kind::Thermal:
        Thermal(grid, weight, op);
        break;
    }
    const float strength = std::clamp(op.strength, 0.f, 1.f);
    bool changed = false;
    for (size_t i = 0; i < grid.heights.size(); ++i)
    {
        const float blended = before[i] + (grid.heights[i] - before[i]) * weight[i] * strength;
        grid.heights[i] = std::isfinite(blended) ? blended : before[i];
        changed |= grid.heights[i] != before[i];
    }
    return changed;
}

std::set<TerrainManager::Cell> TerrainManager::LoadCellsForEdit(double minX, double minZ,
    double maxX, double maxZ)
{
    std::set<Cell> targets;
    const double spanX = double(cellsX) * cellWidth, spanZ = double(cellsY) * cellDepth;
    if (!(spanX > 0.0) || !(spanZ > 0.0)) return targets;
    const auto cellFloor = [](double coordinate) {
        return static_cast<int64_t>(std::floor(std::clamp(coordinate,
            double(std::numeric_limits<int>::min()), double(std::numeric_limits<int>::max()))));
    };
    const int64_t x0 = cellFloor(minX / spanX), x1 = cellFloor(maxX / spanX);
    const int64_t z0 = cellFloor(minZ / spanZ), z1 = cellFloor(maxZ / spanZ);
    if ((x1 - x0 + 1) * (z1 - z0 + 1) > 4096)
    {
        for (Cell cell : GetCells())
            if (cell.first >= x0 && cell.first <= x1 && cell.second >= z0 && cell.second <= z1)
                targets.insert(cell);
    }
    else
        for (int64_t z = z0; z <= z1; ++z)
            for (int64_t x = x0; x <= x1; ++x)
                if (HasCell(int(x), int(z))) targets.emplace(int(x), int(z));
    for (Cell cell : targets)
        if (!GetLoadedCell(cell.first, cell.second))
            LoadCell(cell, m_haveLastCenter
                ? DesiredLodStride(cell, m_lastCenter.first, m_lastCenter.second) : 1);
    return targets;
}

bool TerrainManager::EditRegion(const TerrainRegionMask& mask,
    const TerrainRegionOperation& operation, bool realignObjects)
{
    if (Owner && Owner->GetScene() && Owner->GetScene()->HasStarted()) return false;
    glm::dvec2 minimum, maximum;
    if (!mask.Bounds(minimum, maximum) || cellWidth <= 0.f || cellDepth <= 0.f) return false;
    const double margin = operation.Margin(std::max(cellWidth, cellDepth));
    minimum -= glm::dvec2(margin);
    maximum += glm::dvec2(margin);
    // Global sample indices of the window (cells share border samples).
    const int64_t col0 = int64_t(std::floor(minimum.x / cellWidth));
    const int64_t col1 = int64_t(std::ceil(maximum.x / cellWidth));
    const int64_t row0 = int64_t(std::floor(minimum.y / cellDepth));
    const int64_t row1 = int64_t(std::ceil(maximum.y / cellDepth));
    const int64_t width = col1 - col0 + 1, height = row1 - row0 + 1;
    if (width < 2 || height < 2 || width * height > int64_t(4096) * 4096) return false;
    const std::set<Cell> targets = LoadCellsForEdit(col0 * double(cellWidth),
        row0 * double(cellDepth), col1 * double(cellWidth), row1 * double(cellDepth));
    if (targets.empty()) return false;

    TerrainHeightGrid grid;
    grid.originX = col0 * double(cellWidth);
    grid.originZ = row0 * double(cellDepth);
    grid.spacingX = cellWidth;
    grid.spacingZ = cellDepth;
    grid.width = int(width);
    grid.height = int(height);
    grid.heights.assign(size_t(width * height), 0.f);
    grid.valid.assign(grid.heights.size(), 0);
    const size_t expected = size_t(cellsX + 1) * (cellsY + 1);
    for (Cell cell : targets)
    {
        const auto* terrain = GetLoadedCell(cell.first, cell.second);
        if (!terrain || terrain->heights.size() != expected) continue;
        for (int row = 0; row <= cellsY; ++row)
        {
            const int64_t gy = int64_t(cell.second) * cellsY + row - row0;
            if (gy < 0 || gy >= height) continue;
            for (int col = 0; col <= cellsX; ++col)
            {
                const int64_t gx = int64_t(cell.first) * cellsX + col - col0;
                if (gx < 0 || gx >= width) continue;
                const size_t i = size_t(gy * width + gx);
                grid.heights[i] = terrain->heights[size_t(row) * (cellsX + 1) + col];
                grid.valid[i] = 1;
            }
        }
    }
    FillMissingSamples(grid);
    if (!ApplyRegionOperation(grid, mask, operation)) return false;

    const double spanX = double(cellsX) * cellWidth, spanZ = double(cellsY) * cellDepth;
    std::set<Cell> changed;
    for (Cell cell : targets)
        if (auto* terrain = GetLoadedCell(cell.first, cell.second))
            if (terrain->WriteHeightGrid(grid, cell.first * spanX, cell.second * spanZ,
                    realignObjects))
                changed.insert(cell);
    RecordEditedCells(changed);
    return !changed.empty();
}

bool TerrainManager::PaintRegion(const TerrainRegionMask& mask, float strength, int layer,
    bool erase)
{
    if (Owner && Owner->GetScene() && Owner->GetScene()->HasStarted()) return false;
    glm::dvec2 minimum, maximum;
    if (!mask.Bounds(minimum, maximum) || layer < 0) return false;
    const std::set<Cell> targets = LoadCellsForEdit(minimum.x, minimum.y, maximum.x, maximum.y);
    const double spanX = double(cellsX) * cellWidth, spanZ = double(cellsY) * cellDepth;
    std::set<Cell> changed;
    for (Cell cell : targets)
    {
        auto* terrain = GetLoadedCell(cell.first, cell.second);
        if (!terrain) continue;
        // Authored cells adopt the manager's paint detail, as with the brush.
        if (paintResolution > 0 && !m_proceduralCells.count(cell) &&
            terrain->paintResolution != paintResolution)
            terrain->SetPaintResolution(paintResolution);
        if (terrain->PaintRegion(mask, strength, layer, erase, cell.first * spanX,
                cell.second * spanZ))
            changed.insert(cell);
    }
    RecordEditedCells(changed);
    return !changed.empty();
}
}

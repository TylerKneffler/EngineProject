#include "Core/Components/Terrain/TerrainVolumeBuild.h"

#include "Core/Components/Terrain/Terrain.h"
#include "Core/Components/Terrain/TerrainStamp.h"
#include "Core/Components/Terrain/TerrainSurfaceSample.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace Engine::Components
{
namespace
{
constexpr float StampDepth = 6.f;    // metres of rock behind a stamp's face
constexpr float SmoothUnion = 1.5f;  // fillet where ground and rock meet
constexpr float HashCell = 2.f;      // metres per stamp-point hash cell
// Nearest-rock search radius: past the union's fillet the rock cannot change
// the result, so -StampReach stands for "farther".
constexpr float StampReach = 2.f;

float Smooth(float a, float b)
{
    const float h = std::clamp(.5f + .5f * (a - b) / SmoothUnion, 0.f, 1.f);
    return b + (a - b) * h + SmoothUnion * h * (1.f - h);
}

struct RockPoint
{
    glm::vec3 position, normal;
    float size;
};

struct Rock
{
    std::vector<RockPoint> points;
    std::unordered_map<int64_t, std::vector<uint32_t>> cells;
    glm::vec3 minimum{1e30f}, maximum{-1e30f};

    static int64_t Key(int x, int y, int z)
    {
        return (int64_t(x) & 0x1fffff) | ((int64_t(y) & 0x1fffff) << 21) | ((int64_t(z) & 0x1fffff) << 42);
    }
    void Add(const RockPoint& point)
    {
        const glm::ivec3 cell = glm::ivec3(glm::floor(point.position / HashCell));
        cells[Key(cell.x, cell.y, cell.z)].push_back(static_cast<uint32_t>(points.size()));
        points.push_back(point);
        minimum = glm::min(minimum, point.position);
        maximum = glm::max(maximum, point.position);
    }
    bool Near(const glm::vec3& low, const glm::vec3& high) const
    {
        const float pad = StampDepth + StampReach;
        return !points.empty() && glm::all(glm::lessThanEqual(low, maximum + pad)) &&
            glm::all(glm::greaterThanEqual(high, minimum - pad));
    }
    // Density of the rock at p: depth behind the nearest face point where p
    // lies over the patch (positive inside), else minus the distance to it.
    float Density(const glm::vec3& p) const
    {
        const glm::ivec3 centre = glm::ivec3(glm::floor(p / HashCell));
        const int rings = static_cast<int>(std::ceil(StampReach / HashCell));
        float best = StampReach * StampReach;
        const RockPoint* nearest = nullptr;
        for (int dz = -rings; dz <= rings; ++dz)
            for (int dy = -rings; dy <= rings; ++dy)
                for (int dx = -rings; dx <= rings; ++dx)
                {
                    const auto found = cells.find(Key(centre.x + dx, centre.y + dy, centre.z + dz));
                    if (found == cells.end()) continue;
                    for (uint32_t index : found->second)
                    {
                        const glm::vec3 d = p - points[index].position;
                        const float distanceSquared = glm::dot(d, d);
                        if (distanceSquared < best) { best = distanceSquared; nearest = &points[index]; }
                    }
                }
        if (!nearest) return -StampReach;
        const glm::vec3 offset = p - nearest->position;
        const float along = glm::dot(offset, nearest->normal);
        const float lateral = glm::length(offset - along * nearest->normal);
        const float depth = -along;
        if (lateral <= 1.5f * nearest->size && depth > 0.f && depth < StampDepth)
            return std::min(depth, StampDepth - depth);
        return -std::sqrt(best);
    }
};

// A sculpted cell's surface as a density: the distance to the nearest point
// on the sculpt's finest triangles, positive below the surface (inside the
// ground) by the vertex normals interpolated there. Triangles cover the
// surface however far apart a simplified mesh's vertices are (the nearest
// vertex's plane left holes in the escarpment where vertices were sparse).
// Triangles are hashed by their bounds in SculptCell cells; past SculptReach
// the caller falls back to the heights.
constexpr float SculptCell = 4.f;
constexpr float SculptReach = 8.f;

glm::vec3 ClosestOnTriangle(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b,
    const glm::vec3& c, glm::vec3& weights)
{
    // Ericson, Real-Time Collision Detection, 5.1.5.
    const glm::vec3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0.f && d2 <= 0.f) { weights = {1, 0, 0}; return a; }
    const glm::vec3 bp = p - b;
    const float d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0.f && d4 <= d3) { weights = {0, 1, 0}; return b; }
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f)
    { const float v = d1 / (d1 - d3); weights = {1 - v, v, 0}; return a + v * ab; }
    const glm::vec3 cp = p - c;
    const float d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0.f && d5 <= d6) { weights = {0, 0, 1}; return c; }
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.f && d2 >= 0.f && d6 <= 0.f)
    { const float w = d2 / (d2 - d6); weights = {1 - w, 0, w}; return a + w * ac; }
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.f && (d4 - d3) >= 0.f && (d5 - d6) >= 0.f)
    {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        weights = {0, 1 - w, w};
        return b + w * (c - b);
    }
    const float denominator = 1.f / (va + vb + vc);
    const float v = vb * denominator, w = vc * denominator;
    weights = {1 - v - w, v, w};
    return a + ab * v + ac * w;
}

// The sculpted cells reaching this one, as one triangle soup in its space.
struct SculptSurface
{
    std::vector<glm::vec3> corners;   // three per triangle
    std::vector<glm::vec4> footprints;   // each sculpted cell: x0, z0, x1, z1
    std::unordered_map<int64_t, std::vector<uint32_t>> cells;   // first corners
    std::unordered_map<int64_t, std::vector<uint32_t>> columns; // by x, z bounds only
    float lowest = 1e30f, highest = -1e30f;

    // Adds a sculpt of a width x depth cell, placed by `transform`, keeping
    // the triangles that reach into `keep` (x0, z0, x1, z1).
    void Add(const TerrainSculpt& sculpt, const glm::mat4& transform, float width, float depth,
        const glm::vec4& keep)
    {
        if (sculpt.vertices.empty() || sculpt.levels.empty()) return;
        const glm::vec3 origin(transform[3]);
        footprints.emplace_back(origin.x, origin.z, origin.x + width, origin.z + depth);
        const auto& triangles = sculpt.levels.front();
        for (size_t t = 0; t + 2 < triangles.size(); t += 3)
        {
            glm::vec3 p[3];
            for (int k = 0; k < 3; ++k)
                p[k] = glm::vec3(transform * glm::vec4(sculpt.vertices[triangles[t + k]].position, 1.f));
            const glm::vec3 low = glm::min(p[0], glm::min(p[1], p[2]));
            const glm::vec3 high = glm::max(p[0], glm::max(p[1], p[2]));
            if (high.x < keep.x || low.x > keep.z || high.z < keep.y || low.z > keep.w) continue;
            const auto first = static_cast<uint32_t>(corners.size());
            for (int k = 0; k < 3; ++k) corners.push_back(p[k]);
            lowest = std::min(lowest, low.y);
            highest = std::max(highest, high.y);
            const glm::ivec3 a = glm::ivec3(glm::floor(low / SculptCell));
            const glm::ivec3 b = glm::ivec3(glm::floor(high / SculptCell));
            for (int z = a.z; z <= b.z; ++z)
                for (int x = a.x; x <= b.x; ++x)
                {
                    columns[Rock::Key(x, 0, z)].push_back(first);
                    for (int y = a.y; y <= b.y; ++y) cells[Rock::Key(x, y, z)].push_back(first);
                }
        }
    }
    bool Empty() const { return corners.empty(); }
    // Inside a sculpted cell's footprint: there the sculpt is the ground.
    bool Covers(float x, float z) const
    {
        for (const auto& f : footprints)
            if (x >= f.x - 1e-3f && x <= f.z + 1e-3f && z >= f.y - 1e-3f && z <= f.w + 1e-3f) return true;
        return false;
    }
    // Inside (under the sheet): an odd number of triangles cross the
    // vertical line above p. Folds (turned-over quads) add crossings in
    // pairs, so they cannot flip it; the nearest triangle's normal could, and
    // coarse levels turned one wrong sample into a 16 m hole in the
    // escarpment. A point past the sheet's edge counts as under it when the
    // heights say so.
    bool Inside(const glm::vec3& p, bool fallback) const
    {
        // Nudged off grid-aligned edges and vertices.
        const float x = p.x + 1.37e-3f, z = p.z + 2.11e-3f;
        const auto column = columns.find(Rock::Key(int(std::floor(x / SculptCell)), 0,
            int(std::floor(z / SculptCell))));
        if (column == columns.end()) return fallback;
        int crossings = 0;
        bool covered = false;
        for (uint32_t t : column->second)
        {
            const glm::vec3& a = corners[t];
            const glm::vec3& b = corners[t + 1];
            const glm::vec3& c = corners[t + 2];
            const float area = (b.x - a.x) * (c.z - a.z) - (c.x - a.x) * (b.z - a.z);
            if (std::abs(area) < 1e-9f) continue;   // edge-on: its neighbours carry the crossing
            const float u = ((b.x - x) * (c.z - z) - (c.x - x) * (b.z - z)) / area;
            const float v = ((c.x - x) * (a.z - z) - (a.x - x) * (c.z - z)) / area;
            const float w = 1.f - u - v;
            if (u < 0.f || v < 0.f || w < 0.f) continue;
            covered = true;
            if (a.y * u + b.y * v + c.y * w > p.y) ++crossings;
        }
        return covered ? (crossings & 1) != 0 : fallback;
    }
    // The signed distance to the nearest triangle (SculptReach at most);
    // `density` comes in as the heights' estimate, for the sign past the
    // sheet's edge.
    void Density(const glm::vec3& p, float& density) const
    {
        const glm::ivec3 centre = glm::ivec3(glm::floor(p / SculptCell));
        float best = SculptReach * SculptReach;
        bool found = false;
        for (int rings = 1; rings <= 2 && !found; ++rings)
            for (int dz = -rings; dz <= rings; ++dz)
                for (int dy = -rings; dy <= rings; ++dy)
                    for (int dx = -rings; dx <= rings; ++dx)
                    {
                        if (rings == 2 && std::max({std::abs(dx), std::abs(dy), std::abs(dz)}) < 2) continue;
                        const auto cell = cells.find(Rock::Key(centre.x + dx, centre.y + dy, centre.z + dz));
                        if (cell == cells.end()) continue;
                        for (uint32_t t : cell->second)
                        {
                            glm::vec3 weights;
                            const glm::vec3 q = ClosestOnTriangle(p, corners[t], corners[t + 1], corners[t + 2], weights);
                            const float distanceSquared = glm::dot(p - q, p - q);
                            if (distanceSquared >= best) continue;
                            best = distanceSquared;
                            found = true;
                        }
                    }
        const bool inside = Inside(p, density > 0.f);
        const float distance = found ? std::sqrt(best) : SculptReach;
        density = inside ? distance : -distance;
    }
};

float SmoothMax(float a, float b, float k)
{
    if (k <= 0.f) return std::max(a, b);
    const float h = std::clamp(.5f + .5f * (a - b) / k, 0.f, 1.f);
    return b + (a - b) * h + k * h * (1.f - h);
}
float SmoothMin(float a, float b, float k) { return -SmoothMax(-a, -b, k); }
}

float TerrainVolumeEdit::Distance(const glm::vec3& p) const
{
    switch (shape)
    {
    case Shape::Sphere:
        return glm::length(p - a) - radius;
    case Shape::Capsule:
    {
        const glm::vec3 ab = b - a;
        const float length = glm::dot(ab, ab);
        const float t = length > 0.f ? std::clamp(glm::dot(p - a, ab) / length, 0.f, 1.f) : 0.f;
        return glm::length(p - (a + ab * t)) - radius;
    }
    case Shape::Box:
    {
        const float angle = glm::radians(yaw), c = std::cos(angle), s = std::sin(angle);
        const glm::vec3 d = p - a;
        // Into the box's frame (turned back by yaw about y).
        const glm::vec3 local(c * d.x - s * d.z, d.y, s * d.x + c * d.z);
        const glm::vec3 inner = glm::max(b - glm::vec3(radius), glm::vec3(0.f));
        const glm::vec3 q = glm::abs(local) - inner;
        return glm::length(glm::max(q, glm::vec3(0.f))) + std::min(std::max(q.x, std::max(q.y, q.z)), 0.f) - radius;
    }
    }
    return 1e30f;
}

glm::vec3 TerrainVolumeEdit::Minimum() const
{
    const float pad = blend + 1e-3f;
    switch (shape)
    {
    case Shape::Sphere: return a - glm::vec3(radius + pad);
    case Shape::Capsule: return glm::min(a, b) - glm::vec3(radius + pad);
    case Shape::Box:
    {
        const float r = glm::length(glm::vec2(b.x, b.z));   // any yaw
        return a - glm::vec3(r, b.y, r) - glm::vec3(pad);
    }
    }
    return a;
}

glm::vec3 TerrainVolumeEdit::Maximum() const
{
    const float pad = blend + 1e-3f;
    switch (shape)
    {
    case Shape::Sphere: return a + glm::vec3(radius + pad);
    case Shape::Capsule: return glm::max(a, b) + glm::vec3(radius + pad);
    case Shape::Box:
    {
        const float r = glm::length(glm::vec2(b.x, b.z));
        return a + glm::vec3(r, b.y, r) + glm::vec3(pad);
    }
    }
    return a;
}

TerrainVolumeEdit TerrainVolumeEdit::Shifted(const glm::vec3& offset) const
{
    TerrainVolumeEdit edit = *this;
    edit.a += offset;
    if (shape == Shape::Capsule) edit.b += offset;
    return edit;
}

bool LoadTerrainVolumeEdits(const std::string& path, std::vector<TerrainVolumeEdit>& edits)
{
    edits.clear();
    std::ifstream file(path);
    if (!file) return true;
    std::string line;
    while (std::getline(file, line))
    {
        if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        std::istringstream in(line);
        std::string operation, shape;
        if (!(in >> operation)) continue;
        TerrainVolumeEdit edit;
        if (operation == "add") edit.subtract = false;
        else if (operation != "subtract") return false;
        in >> shape;
        bool ok = false;
        if (shape == "sphere")
        {
            edit.shape = TerrainVolumeEdit::Shape::Sphere;
            ok = static_cast<bool>(in >> edit.a.x >> edit.a.y >> edit.a.z >> edit.radius >> edit.blend);
        }
        else if (shape == "capsule")
        {
            edit.shape = TerrainVolumeEdit::Shape::Capsule;
            ok = static_cast<bool>(in >> edit.a.x >> edit.a.y >> edit.a.z >> edit.b.x >> edit.b.y >> edit.b.z >>
                edit.radius >> edit.blend);
        }
        else if (shape == "box")
        {
            edit.shape = TerrainVolumeEdit::Shape::Box;
            ok = static_cast<bool>(in >> edit.a.x >> edit.a.y >> edit.a.z >> edit.b.x >> edit.b.y >> edit.b.z >>
                edit.yaw >> edit.radius >> edit.blend);
        }
        if (!ok) return false;
        edit.radius = std::max(edit.radius, 0.f);
        edit.blend = std::max(edit.blend, 0.f);
        edits.push_back(edit);
    }
    return true;
}

bool SaveTerrainVolumeEdits(const std::string& path, const std::vector<TerrainVolumeEdit>& edits)
{
    std::ofstream file(path, std::ios::trunc);
    if (!file) return false;
    file << "# Volume terrain edits (TerrainVolumeEdit), cell space, applied in order.\n";
    for (const auto& edit : edits)
    {
        file << (edit.subtract ? "subtract " : "add ");
        switch (edit.shape)
        {
        case TerrainVolumeEdit::Shape::Sphere:
            file << "sphere " << edit.a.x << ' ' << edit.a.y << ' ' << edit.a.z << ' ' << edit.radius;
            break;
        case TerrainVolumeEdit::Shape::Capsule:
            file << "capsule " << edit.a.x << ' ' << edit.a.y << ' ' << edit.a.z << ' ' << edit.b.x << ' '
                << edit.b.y << ' ' << edit.b.z << ' ' << edit.radius;
            break;
        case TerrainVolumeEdit::Shape::Box:
            file << "box " << edit.a.x << ' ' << edit.a.y << ' ' << edit.a.z << ' ' << edit.b.x << ' '
                << edit.b.y << ' ' << edit.b.z << ' ' << edit.yaw << ' ' << edit.radius;
            break;
        }
        file << ' ' << edit.blend << '\n';
    }
    return static_cast<bool>(file);
}

TerrainVolumeInput TerrainVolumeInput::From(const Terrain& terrain)
{
    TerrainVolumeInput input;
    input.cellsX = terrain.cellsX; input.cellsY = terrain.cellsY;
    input.cellWidth = terrain.cellWidth; input.cellDepth = terrain.cellDepth;
    input.heights = terrain.heights;
    for (const auto& instance : terrain.stamps)
    {
        if (instance.stamp) input.stamps.push_back(instance);
        if (instance.sculpt) input.sculpts.push_back(instance);
    }
    return input;
}

std::shared_ptr<TerrainVolume> BuildTerrainVolume(const Terrain& terrain, float spacing, float band)
{
    return BuildTerrainVolume(TerrainVolumeInput::From(terrain), spacing, band);
}

std::shared_ptr<TerrainVolume> BuildTerrainVolume(const TerrainVolumeInput& terrain, float spacing, float band)
{
    const int cellsX = terrain.cellsX, cellsY = terrain.cellsY;
    if (cellsX < 1 || cellsY < 1 || terrain.heights.size() != size_t(cellsX + 1) * (cellsY + 1) ||
        spacing <= 0.f)
        return nullptr;
    const float width = cellsX * terrain.cellWidth, depth = cellsY * terrain.cellDepth;
    // A margin past the cell on every side, as wide as the coarsest level's
    // cell (VolumeTerrain samples every 8th sample there): surface nets puts
    // vertices at cell centres and drops cells without samples on both
    // sides, so without it a coarse level stopped up to a coarse cell short
    // of the edge and neighbouring cells left gaps (the escarpment at 250 m;
    // one sample only closed the gap at full detail). The margin is read,
    // not meshed: chunks cover the interior, the cell itself. Past the edge
    // the ground is the neighbours' (their heights, sculpts and stamps), so a
    // coarse level bends there as theirs does and both meet.
    constexpr int Margin = 9;
    const float reach = Margin * spacing;

    // The heights with the neighbours' rows around them, `ring` points deep;
    // a missing neighbour repeats the border.
    const int ring = static_cast<int>(std::ceil(reach / std::min(terrain.cellWidth, terrain.cellDepth))) + 1;
    const int wideX = cellsX + 2 * ring, wideY = cellsY + 2 * ring;
    std::vector<float> heights(size_t(wideX + 1) * (wideY + 1));
    const size_t cellPoints = terrain.heights.size();
    for (int row = 0; row <= wideY; ++row)
        for (int col = 0; col <= wideX; ++col)
        {
            int x = col - ring, z = row - ring;
            const int dx = x < 0 ? -1 : x > cellsX ? 1 : 0;
            const int dz = z < 0 ? -1 : z > cellsY ? 1 : 0;
            const auto& source = terrain.neighbourHeights[size_t((dz + 1) * 3 + dx + 1)];
            const std::vector<float>* from = &terrain.heights;
            if ((dx || dz) && source.size() == cellPoints) { from = &source; x -= dx * cellsX; z -= dz * cellsY; }
            x = std::clamp(x, 0, cellsX);
            z = std::clamp(z, 0, cellsY);
            heights[size_t(row) * (wideX + 1) + col] = (*from)[size_t(z) * (cellsX + 1) + x];
        }
    const float shiftX = ring * terrain.cellWidth, shiftZ = ring * terrain.cellDepth;
    const auto heightAt = [&](float x, float z, glm::vec3* normal)
    {
        // SampleTerrainSurface takes the grid's spacing, not the cell's span.
        float h = 0.f;
        SampleTerrainSurface(heights, wideX, wideY, terrain.cellWidth, terrain.cellDepth,
            x + shiftX, z + shiftZ, h, normal);
        return h;
    };

    // The rock: every placed stamp's inside points, in cell space.
    Rock rock;
    for (const auto& instance : terrain.stamps)
    {
        const TerrainStamp* stamp = instance.stamp.get();
        if (!stamp) continue;
        const glm::mat3 linear(instance.transform);
        const glm::mat3 normalMatrix = glm::transpose(glm::inverse(linear));
        const float size = stamp->spacing * std::cbrt(std::abs(glm::determinant(linear)));
        for (const auto& point : stamp->points)
        {
            if (point.weight <= .5f) continue;
            RockPoint rockPoint;
            rockPoint.position = glm::vec3(instance.transform * glm::vec4(point.position, 1.f));
            rockPoint.normal = glm::normalize(normalMatrix * point.normal);
            rockPoint.size = size;
            rock.Add(rockPoint);
        }
    }
    // The sculpts reaching the cell or its margin.
    SculptSurface sculpt;
    {
        const float keep = reach + SculptReach + SculptCell;
        for (const auto& instance : terrain.sculpts)
            if (instance.sculpt)
                sculpt.Add(*instance.sculpt, instance.transform, width, depth,
                    glm::vec4(-keep, -keep, width + keep, depth + keep));
    }
    const bool sculpted = !sculpt.Empty();

    // The height range of everything in the grid's footprint.
    float lowest = 1e30f, highest = -1e30f;
    {
        const int pad = static_cast<int>(std::ceil(reach / std::min(terrain.cellWidth, terrain.cellDepth)));
        for (int row = ring - pad; row <= ring + cellsY + pad; ++row)
            for (int col = ring - pad; col <= ring + cellsX + pad; ++col)
            {
                const float h = heights[size_t(std::clamp(row, 0, wideY)) * (wideX + 1) + std::clamp(col, 0, wideX)];
                lowest = std::min(lowest, h);
                highest = std::max(highest, h);
            }
    }
    if (sculpted)
    {
        lowest = std::min(lowest, sculpt.lowest);
        highest = std::max(highest, sculpt.highest);
    }
    if (!rock.points.empty())
    {
        lowest = std::min(lowest, rock.minimum.y);
        highest = std::max(highest, rock.maximum.y);
    }
    // Edits that reach the grid's footprint.
    std::vector<TerrainVolumeEdit> edits;
    for (const auto& edit : terrain.edits)
    {
        const glm::vec3 low = edit.Minimum(), high = edit.Maximum();
        if (high.x < -reach || low.x > width + reach || high.z < -reach || low.z > depth + reach) continue;
        edits.push_back(edit);
        lowest = std::min(lowest, low.y + band);
        highest = std::max(highest, high.y - band);
    }

    auto volume = std::make_shared<TerrainVolume>();
    volume->spacing = spacing;
    const int cellsAcross = static_cast<int>(std::lround(width / spacing));
    const int cellsDeep = static_cast<int>(std::lround(depth / spacing));
    volume->sizeX = cellsAcross + 1 + 2 * Margin;
    volume->sizeZ = cellsDeep + 1 + 2 * Margin;
    // The bottom on a multiple of a storage chunk (32 samples, a multiple of
    // every level's cell and of VolumeTerrain's chunks), so every cell
    // samples one world lattice and cuts it into chunks in the same places:
    // with the bottom at each cell's own lowest point less the band,
    // neighbours' samples sat at different heights and their coarse vertices
    // missed each other along the shared edge.
    const float step = 32.f * spacing;
    const float bottom = std::floor((lowest - band) / step) * step;
    volume->sizeY = static_cast<int>(std::ceil((highest + band - bottom) / spacing)) + 1;
    volume->origin = {-Margin * spacing, bottom, -Margin * spacing};
    volume->interiorFirst = {Margin, 0, Margin};
    volume->interiorCells = {cellsAcross, volume->sizeY - 1, cellsDeep};
    volume->unitsPerMetre = 256.f;
    const int c = volume->chunkSamples = 32;
    volume->chunksX = (volume->sizeX + c - 1) / c;
    volume->chunksY = (volume->sizeY + c - 1) / c;
    volume->chunksZ = (volume->sizeZ + c - 1) / c;
    volume->chunks.resize(size_t(volume->chunksX) * volume->chunksY * volume->chunksZ);
    const float limit = band;
    const auto quantize = [&](float metres)
    { return static_cast<int16_t>(std::lround(std::clamp(metres, -limit, limit) * volume->unitsPerMetre)); };
    std::vector<const TerrainVolumeEdit*> chunkEdits;
    for (int cy = 0; cy < volume->chunksY; ++cy)
        for (int cz = 0; cz < volume->chunksZ; ++cz)
            for (int cx = 0; cx < volume->chunksX; ++cx)
            {
                auto& chunk = volume->chunks[(size_t(cy) * volume->chunksZ + cz) * volume->chunksX + cx];
                const glm::vec3 low = volume->origin + glm::vec3(cx, cy, cz) * float(c) * spacing;
                const glm::vec3 high = low + glm::vec3(float(c - 1) * spacing);
                // The ground's height range over the chunk's footprint.
                float groundLow = 1e30f, groundHigh = -1e30f;
                const int col0 = std::clamp(int(std::floor((low.x + shiftX) / terrain.cellWidth)) - 1, 0, wideX);
                const int col1 = std::clamp(int(std::ceil((high.x + shiftX) / terrain.cellWidth)) + 1, 0, wideX);
                const int row0 = std::clamp(int(std::floor((low.z + shiftZ) / terrain.cellDepth)) - 1, 0, wideY);
                const int row1 = std::clamp(int(std::ceil((high.z + shiftZ) / terrain.cellDepth)) + 1, 0, wideY);
                for (int row = row0; row <= row1; ++row)
                    for (int col = col0; col <= col1; ++col)
                    {
                        const float h = heights[size_t(row) * (wideX + 1) + col];
                        groundLow = std::min(groundLow, h);
                        groundHigh = std::max(groundHigh, h);
                    }
                // A sculpt can stand far from the heights: no shortcut but
                // its overall height range.
                if (sculpted)
                {
                    groundLow = std::min(groundLow, sculpt.lowest);
                    groundHigh = std::max(groundHigh, sculpt.highest);
                }
                chunkEdits.clear();
                for (const auto& edit : edits)
                {
                    const glm::vec3 a = edit.Minimum(), b = edit.Maximum();
                    if (glm::all(glm::lessThanEqual(a, high)) && glm::all(glm::greaterThanEqual(b, low)))
                        chunkEdits.push_back(&edit);
                }
                const bool nearRock = rock.Near(low, high);
                const bool plain = !nearRock && chunkEdits.empty();
                if (plain && low.y > groundHigh + band) { chunk.uniform = quantize(-limit); continue; }
                if (plain && high.y < groundLow - band) { chunk.uniform = quantize(limit); continue; }
                chunk.samples.resize(size_t(c) * c * c);
                bool uniform = true;
                for (int y = 0; y < c; ++y)
                    for (int z = 0; z < c; ++z)
                        for (int x = 0; x < c; ++x)
                        {
                            const glm::vec3 p = low + glm::vec3(x, y, z) * spacing;
                            glm::vec3 normal(0.f, 1.f, 0.f);
                            const float h = heightAt(p.x, p.z, &normal);
                            float d = (h - p.y) * std::max(normal.y, .05f);
                            // In a sculpted cell (this one or a neighbour)
                            // the sculpt is the ground.
                            if (sculpted && sculpt.Covers(p.x, p.z)) sculpt.Density(p, d);
                            // Rock reaches at most StampDepth / 2 inside; deeper
                            // ground is unchanged by the union.
                            if (nearRock && d < StampDepth * .5f + SmoothUnion)
                                d = Smooth(d, rock.Density(p));
                            for (const auto* edit : chunkEdits)
                            {
                                const float shape = edit->Distance(p);
                                d = edit->subtract ? SmoothMin(d, shape, edit->blend)
                                                   : SmoothMax(d, -shape, edit->blend);
                            }
                            const int16_t value = quantize(d);
                            chunk.samples[(size_t(y) * c + z) * c + x] = value;
                            uniform = uniform && value == chunk.samples[0];
                        }
                if (uniform) { chunk.uniform = chunk.samples[0]; chunk.samples.clear(); }
            }
    return volume;
}
}

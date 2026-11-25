#include "Core/Components/Terrain/TerrainVolumeBuild.h"

#include "Core/Components/Terrain/Terrain.h"
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
    // the ground is the neighbours' (their heights), so a
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
    // The top on that lattice too: VolumeTerrain's chunks are 32 cells tall
    // and a coarse level's cells span up to 32 samples, so a top part-way
    // through one left its last coarse cell reading past the volume. Where
    // the ground crossed that cell it had no vertex: holes in the coarsest
    // levels (meadow cell 4,-9 from 500 m, 1,507 of 16,384 columns).
    const int cellsTall = std::max(1, static_cast<int>(std::ceil((highest + band - bottom) / step))) * 32;
    volume->sizeY = cellsTall + 1;
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
                chunkEdits.clear();
                for (const auto& edit : edits)
                {
                    const glm::vec3 a = edit.Minimum(), b = edit.Maximum();
                    if (glm::all(glm::lessThanEqual(a, high)) && glm::all(glm::greaterThanEqual(b, low)))
                        chunkEdits.push_back(&edit);
                }
                const bool plain = chunkEdits.empty();
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
